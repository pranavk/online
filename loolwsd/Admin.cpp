/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4; fill-column: 100 -*- */
/*
 * This file is part of the LibreOffice project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include <cassert>
#include <mutex>
#include <sys/poll.h>

#include <Poco/Net/HTTPCookie.h>
#include <Poco/Net/HTTPBasicCredentials.h>
#include <Poco/Net/HTTPRequest.h>
#include <Poco/Net/HTTPRequestHandler.h>
#include <Poco/Net/HTTPServerParams.h>
#include <Poco/Net/HTTPServerRequest.h>
#include <Poco/Net/HTTPServerResponse.h>
#include <Poco/Net/NetException.h>
#include <Poco/Net/SecureServerSocket.h>
#include <Poco/Net/WebSocket.h>
#include <Poco/StringTokenizer.h>
#include <Poco/Util/ServerApplication.h>
#include <Poco/Util/Timer.h>

#include "Auth.hpp"
#include "Admin.hpp"
#include "AdminModel.hpp"
#include "Common.hpp"
#include "FileServer.hpp"
#include "TileCache.hpp"
#include "Storage.hpp"
#include "LOOLProtocol.hpp"
#include "LOOLWSD.hpp"
#include "IoUtil.hpp"
#include "Unit.hpp"
#include "Util.hpp"

using namespace LOOLProtocol;

using Poco::StringTokenizer;
using Poco::Net::HTTPBasicCredentials;
using Poco::Net::HTTPCookie;
using Poco::Net::HTTPRequest;
using Poco::Net::HTTPRequestHandler;
using Poco::Net::HTTPRequestHandlerFactory;
using Poco::Net::HTTPResponse;
using Poco::Net::HTTPServerParams;
using Poco::Net::HTTPServerRequest;
using Poco::Net::HTTPServerResponse;
using Poco::Net::SecureServerSocket;
using Poco::Net::ServerSocket;
using Poco::Net::Socket;
using Poco::Net::WebSocket;
using Poco::Net::WebSocketException;
using Poco::Util::Application;

/// Handle admin requests.
void AdminRequestHandler::handleWSRequests(HTTPServerRequest& request, HTTPServerResponse& response, int nSessionId)
{
    try
    {
        auto ws = std::make_shared<WebSocket>(request, response);

        {
            std::unique_lock<std::mutex> modelLock(_admin->getLock());
            // Subscribe the websocket of any AdminModel updates
            AdminModel& model = _admin->getModel();
            model.subscribe(nSessionId, ws);
        }

        const Poco::Timespan waitTime(POLL_TIMEOUT_MS * 1000);
        int flags = 0;
        int n = 0;
        ws->setReceiveTimeout(0);
        do
        {
            char buffer[200000]; //FIXME: Dynamic?

            if (ws->poll(waitTime, Socket::SELECT_READ))
            {
                n = ws->receiveFrame(buffer, sizeof(buffer), flags);

                if ((flags & WebSocket::FRAME_OP_BITMASK) == WebSocket::FRAME_OP_PING)
                {
                    // Echo back the ping payload as pong.
                    // Technically, we should send back a PONG control frame.
                    // However Firefox (probably) or Node.js (possibly) doesn't
                    // like that and closes the socket when we do.
                    // Echoing the payload as a normal frame works with Firefox.
                    ws->sendFrame(buffer, n /*, WebSocket::FRAME_OP_PONG*/);
                }
                else if ((flags & WebSocket::FRAME_OP_BITMASK) == WebSocket::FRAME_OP_PONG)
                {
                    // In case we do send pings in the future.
                }
                else if (n <= 0)
                {
                    // Connection closed.
                    Log::warn() << "Received " << n
                                << " bytes. Connection closed. Flags: "
                                << std::hex << flags << Log::end;
                    break;
                }
                else
                {
                    assert(n > 0);
                    const std::string firstLine = getFirstLine(buffer, n);
                    StringTokenizer tokens(firstLine, " ", StringTokenizer::TOK_IGNORE_EMPTY | StringTokenizer::TOK_TRIM);
                    Log::trace("Recv: " + firstLine);

                    if (tokens.count() < 1)
                        continue;

                    // Lock the model mutex before interacting with it
                    std::unique_lock<std::mutex> modelLock(_admin->getLock());
                    AdminModel& model = _admin->getModel();

                    if (tokens[0] == "documents" ||
                        tokens[0] == "active_users_count" ||
                        tokens[0] == "active_docs_count" ||
                        tokens[0] == "mem_stats" ||
                        tokens[0] == "cpu_stats" )
                    {
                        const std::string responseFrame = tokens[0] + " " + model.query(tokens[0]);
                        sendTextFrame(ws, responseFrame);
                    }
                    else if (tokens[0] == "subscribe" && tokens.count() > 1)
                    {
                        for (unsigned i = 0; i < tokens.count() - 1; i++)
                        {
                            model.subscribe(nSessionId, tokens[i + 1]);
                        }
                    }
                    else if (tokens[0] == "unsubscribe" && tokens.count() > 1)
                    {
                        for (unsigned i = 0; i < tokens.count() - 1; i++)
                        {
                            model.unsubscribe(nSessionId, tokens[i + 1]);
                        }
                    }
                    else if (tokens[0] == "total_mem")
                    {
                        unsigned totalMem = _admin->getTotalMemoryUsage(model);
                        std::string responseFrame = "total_mem " + std::to_string(totalMem);
                        sendTextFrame(ws, responseFrame);
                    }
                    else if (tokens[0] == "kill" && tokens.count() == 2)
                    {
                        try
                        {
                            const auto pid = std::stoi(tokens[1]);
                            if (kill(pid, SIGINT) != 0 && kill(pid, 0) !=0)
                            {
                                Log::syserror("Cannot terminate PID: " + tokens[0]);
                            }
                        }
                        catch(std::invalid_argument& exc)
                        {
                            Log::warn() << "Invalid PID to kill: " << tokens[0] << Log::end;
                        }
                    }
                    else if (tokens[0] == "settings")
                    {
                        // for now, we have only these settings
                        std::ostringstream oss;
                        oss << tokens[0] << " "
                            << "mem_stats_size=" << model.query("mem_stats_size") << " "
                            << "mem_stats_interval=" << std::to_string(_admin->getMemStatsInterval()) << " "
                            << "cpu_stats_size="  << model.query("cpu_stats_size") << " "
                            << "cpu_stats_interval=" << std::to_string(_admin->getCpuStatsInterval());

                        std::string responseFrame = oss.str();
                        sendTextFrame(ws, responseFrame);
                    }
                    else if (tokens[0] == "set" && tokens.count() > 1)
                    {
                        for (unsigned i = 1; i < tokens.count(); i++)
                        {
                            StringTokenizer setting(tokens[i], "=", StringTokenizer::TOK_IGNORE_EMPTY | StringTokenizer::TOK_TRIM);
                            unsigned settingVal = 0;
                            try
                            {
                                settingVal = std::stoi(setting[1]);
                            }
                            catch (const std::exception& exc)
                            {
                                Log::warn() << "Invalid setting value: "
                                            << setting[1] << " for "
                                            << setting[0] << Log::end;
                                continue;
                            }

                            if (setting[0] == "mem_stats_size")
                            {
                                if (settingVal != static_cast<unsigned>(std::stoi(model.query(setting[0]))))
                                {
                                    model.setMemStatsSize(settingVal);
                                }
                            }
                            else if (setting[0] == "mem_stats_interval")
                            {
                                if (settingVal != _admin->getMemStatsInterval())
                                {
                                    _admin->rescheduleMemTimer(settingVal);
                                    model.clearMemStats();
                                    model.notify("settings mem_stats_interval=" + std::to_string(settingVal));
                                }
                            }
                            else if (setting[0] == "cpu_stats_size")
                            {
                                if (settingVal != static_cast<unsigned>(std::stoi(model.query(setting[0]))))
                                {
                                    model.setCpuStatsSize(settingVal);
                                }
                            }
                            else if (setting[0] == "cpu_stats_interval")
                            {
                                if (settingVal != _admin->getCpuStatsInterval())
                                {
                                    _admin->rescheduleCpuTimer(settingVal);
                                    model.clearCpuStats();
                                    model.notify("settings cpu_stats_interval=" + std::to_string(settingVal));
                                }
                            }
                        }
                    }
                }
            }
        }
        while (!TerminationFlag &&
               (flags & WebSocket::FRAME_OP_BITMASK) != WebSocket::FRAME_OP_CLOSE);
        Log::debug() << "Finishing AdminProcessor. TerminationFlag: " << TerminationFlag
                     << ", payload size: " << n
                     << ", flags: " << std::hex << flags << Log::end;
    }
    catch (const WebSocketException& exc)
    {
        Log::error("AdminRequestHandler::handleRequest: WebSocketException: " + exc.message());
        switch (exc.code())
        {
        case WebSocket::WS_ERR_HANDSHAKE_UNSUPPORTED_VERSION:
            response.set("Sec-WebSocket-Version", WebSocket::WEBSOCKET_VERSION);
            // fallthrough
        case WebSocket::WS_ERR_NO_HANDSHAKE:
        case WebSocket::WS_ERR_HANDSHAKE_NO_VERSION:
        case WebSocket::WS_ERR_HANDSHAKE_NO_KEY:
            response.setStatusAndReason(HTTPResponse::HTTP_BAD_REQUEST);
            response.setContentLength(0);
            response.send();
            break;
        }
    }
    catch (const Poco::Net::NotAuthenticatedException& exc)
    {
        Log::info("NotAuthenticatedException");
        response.set("WWW-Authenticate", "Basic realm=\"ws-online\"");
        response.setStatusAndReason(HTTPResponse::HTTP_UNAUTHORIZED);
        response.setContentLength(0);
        response.send();
    }
    catch (const std::exception& exc)
    {
        Log::error(std::string("AdminRequestHandler::handleRequest: Exception: ") + exc.what());
    }
}

AdminRequestHandler::AdminRequestHandler(Admin* adminManager)
    : _admin(adminManager)
{    }

void AdminRequestHandler::sendTextFrame(std::shared_ptr<Poco::Net::WebSocket>& socket, const std::string& message)
{
    UnitWSD::get().onAdminQueryMessage(message);
    socket->sendFrame(message.data(), message.size());
}

void AdminRequestHandler::handleRequest(HTTPServerRequest& request, HTTPServerResponse& response)
{
    // Different session id pool for admin sessions (?)
    const auto nSessionId = Util::decodeId(LOOLWSD::GenSessionId());

    Util::setThreadName("admin_ws_" + std::to_string(nSessionId));

    Log::debug("Thread started.");

    try
    {
        std::string requestURI = request.getURI();
        StringTokenizer pathTokens(requestURI, "/", StringTokenizer::TOK_IGNORE_EMPTY | StringTokenizer::TOK_TRIM);

        if (request.find("Upgrade") != request.end() && Poco::icompare(request["Upgrade"], "websocket") == 0)
        {
            if (!FileServerRequestHandler::isAdminLoggedIn(request, response))
                throw Poco::Net::NotAuthenticatedException("Invalid admin login");

            handleWSRequests(request, response, nSessionId);
        }
    }
    catch(const Poco::Net::NotAuthenticatedException& exc)
    {
        Log::info("Admin::NotAuthenticated");
        response.set("WWW-Authenticate", "Basic realm=\"online\"");
        response.setStatusAndReason(HTTPResponse::HTTP_UNAUTHORIZED);
        response.setContentLength(0);
        response.send();
    }
    catch (const std::exception& exc)
    {
        Log::info(std::string("Admin::handleRequest: Exception: ") + exc.what());
        response.setStatusAndReason(HTTPResponse::HTTP_BAD_REQUEST);
        response.setContentLength(0);
        response.send();
    }
    Log::debug("Thread finished.");
}

/// An admin command processor.
Admin::Admin() :
    _model(AdminModel())
{
    Log::info("Admin ctor.");

    _memStatsTask = new MemoryStats(this);
    _memStatsTimer.schedule(_memStatsTask, _memStatsTaskInterval, _memStatsTaskInterval);

    _cpuStatsTask = new CpuStats(this);
    _cpuStatsTimer.schedule(_cpuStatsTask, _cpuStatsTaskInterval, _cpuStatsTaskInterval);
}

Admin::~Admin()
{
    Log::info("~Admin dtor.");

    _memStatsTask->cancel();
    _cpuStatsTask->cancel();
}

void Admin::addDoc(const std::string& docKey, Poco::Process::PID pid, const std::string& filename, const std::string& sessionId)
{
    std::unique_lock<std::mutex> modelLock(_modelMutex);
    _model.addDocument(docKey, pid, filename, sessionId);
}

void Admin::rmDoc(const std::string& docKey, const std::string& sessionId)
{
    std::unique_lock<std::mutex> modelLock(_modelMutex);
    _model.removeDocument(docKey, sessionId);
}

void Admin::rmDoc(const std::string& docKey)
{
    std::unique_lock<std::mutex> modelLock(_modelMutex);
    _model.removeDocument(docKey);
}

void MemoryStats::run()
{
    std::unique_lock<std::mutex> modelLock(_admin->getLock());
    AdminModel& model = _admin->getModel();
    unsigned totalMem = _admin->getTotalMemoryUsage(model);

    Log::trace("Total memory used: " + std::to_string(totalMem));
    model.addMemStats(totalMem);
}

void CpuStats::run()
{
    //TODO: Implement me
    //std::unique_lock<std::mutex> modelLock(_admin->getLock());
    //model.addCpuStats(totalMem);
}

void Admin::rescheduleMemTimer(unsigned interval)
{
    _memStatsTask->cancel();
    _memStatsTaskInterval = interval;
    _memStatsTask = new MemoryStats(this);
    _memStatsTimer.schedule(_memStatsTask, _memStatsTaskInterval, _memStatsTaskInterval);
    Log::info("Memory stats interval changed - New interval: " + std::to_string(interval));
}

void Admin::rescheduleCpuTimer(unsigned interval)
{
    _cpuStatsTask->cancel();
    _cpuStatsTaskInterval = interval;
    _cpuStatsTask = new CpuStats(this);
    _cpuStatsTimer.schedule(_cpuStatsTask, _cpuStatsTaskInterval, _cpuStatsTaskInterval);
    Log::info("CPU stats interval changed - New interval: " + std::to_string(interval));
}

unsigned Admin::getTotalMemoryUsage(AdminModel& model)
{
    unsigned totalMem = Util::getMemoryUsage(_forKitPid);
    totalMem += model.getTotalMemoryUsage();
    totalMem += Util::getMemoryUsage(Poco::Process::id());

    return totalMem;
}

unsigned Admin::getMemStatsInterval()
{
    return _memStatsTaskInterval;
}

unsigned Admin::getCpuStatsInterval()
{
    return _cpuStatsTaskInterval;
}

AdminModel& Admin::getModel()
{
    return _model;
}

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
