/*
    Charon - a transport system for GSP data
    Copyright (C) 2019  Autonomous Worlds Ltd

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#include "private/xmppclient.hpp"

#include <glog/logging.h>

#include <chrono>
#include <sstream>

namespace charon
{

namespace
{

/**
 * Waiting time during the receive loop to give other threads a chance on
 * locking the mutex if they want to send.
 */
constexpr auto WAITING_SLEEP = std::chrono::milliseconds (1);

} // anonymous namespace

XmppClient::XmppClient (const gloox::JID& j, const std::string& password)
  : jid(j), client(jid, password)
{
  client.registerConnectionListener (this);
  client.logInstance ().registerLogHandler (gloox::LogLevelDebug,
                                            gloox::LogAreaAll, this);

  /* FIXME:  For some reason, gloox and TLS does not connect to chat.xaya.io
     successfully all the time.  Without TLS it works, so we do that for
     initial development and testing.  But we need to figure this out.  */
  client.setTls (gloox::TLSDisabled);
}

XmppClient::~XmppClient ()
{
  if (recvLoop != nullptr)
    Disconnect ();
}

void
XmppClient::Connect (const int priority)
{
  LOG (INFO)
      << "Connecting to XMPP server with " << jid.full ()
      << " and priority " << priority << "...";
  CHECK (recvLoop == nullptr);

  client.presence ().setPriority (priority);
  connected = false;
  CHECK (client.connect (false));

  stopLoop = false;
  recvLoop = std::make_unique<std::thread> ([this] ()
    {
      while (!stopLoop)
        {
          if (!Receive ())
            return;

          /* Give other threads a chance to lock the mutex if they want to
             do something (e.g. through RunWithClient).  */
          std::this_thread::sleep_for (WAITING_SLEEP);
        }
    });

  while (!connected)
    std::this_thread::sleep_for (WAITING_SLEEP);
}

bool
XmppClient::Receive ()
{
  std::lock_guard<std::recursive_mutex> lock(mut);

  /* This method is called in a loop anyway, with sleeps in between (when not
     holding the mut lock).  Thus it is enough to really only check if there
     are waiting messages here without blocking for any amount of time if not.
     This ensures the lock is not held too much, blocking threads that want
     to send messages instead.  */
  const auto res = client.recv (0);

  switch (res)
    {
    case gloox::ConnNotConnected:
    case gloox::ConnStreamClosed:
      return false;

    case gloox::ConnNoError:
      return true;

    default:
      LOG (FATAL) << "Receive error for " << jid.full () << ": " << res;
    }
}

void
XmppClient::Disconnect ()
{
  LOG (INFO) << "Disconnecting XMPP client " << jid.full () << "...";

  client.disconnect ();
  connected = false;

  if (recvLoop != nullptr)
    {
      stopLoop = true;
      recvLoop->join ();
      recvLoop.reset ();
    }
}

void
XmppClient::onConnect ()
{
  LOG (INFO)
      << "XMPP connection to the server is established for " << jid.full ();
  connected = true;
}

void
XmppClient::onDisconnect (const gloox::ConnectionError err)
{
  LOG (INFO) << "Disconnected from the XMPP server with " << jid.full ();

  switch (err)
    {
    case gloox::ConnStreamClosed:
    case gloox::ConnUserDisconnected:
      break;

    default:
      LOG (FATAL) << "Unexpected disconnect: " << err;
    }
}

bool
XmppClient::onTLSConnect (const gloox::CertInfo& info)
{
  LOG (INFO)
      << "Server presented a certificate for " << info.server
      << " from " << info.issuer;

  LOG_IF (WARNING, info.status != gloox::CertOk)
      << "Certificate status is not ok: " << info.status;
  LOG_IF (WARNING, !info.chain)
      << "Certificate chain is invalid, accepting nevertheless";

  return true;
}

void
XmppClient::handleLog (const gloox::LogLevel level, const gloox::LogArea area,
                       const std::string& msg)
{
  std::ostringstream fullMsg;
  fullMsg << "gloox (" << area << ") for " << jid.full () << ": " << msg;

  switch (level)
    {
    case gloox::LogLevelError:
      LOG (ERROR) << fullMsg.str ();
      break;
    case gloox::LogLevelWarning:
      LOG (WARNING) << fullMsg.str ();
      break;
    default:
      VLOG (1) << fullMsg.str ();
      break;
    }
}

} // namespace charon
