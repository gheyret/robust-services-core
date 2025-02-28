//==============================================================================
//
//  NwDaemons.cpp
//
//  Copyright (C) 2013-2021  Greg Utas
//
//  This file is part of the Robust Services Core (RSC).
//
//  RSC is free software: you can redistribute it and/or modify it under the
//  terms of the GNU General Public License as published by the Free Software
//  Foundation, either version 3 of the License, or (at your option) any later
//  version.
//
//  RSC is distributed in the hope that it will be useful, but WITHOUT ANY
//  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
//  FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
//  details.
//
//  You should have received a copy of the GNU General Public License along
//  with RSC.  If not, see <http://www.gnu.org/licenses/>.
//
#include "NwDaemons.h"
#include <ostream>
#include "DaemonRegistry.h"
#include "Debug.h"
#include "Formatters.h"
#include "Singleton.h"
#include "TcpIoThread.h"
#include "UdpIoThread.h"

using namespace NodeBase;
using std::ostream;
using std::string;

//------------------------------------------------------------------------------

namespace NetworkBase
{
fixed_string TcpIoDaemonName = "tcp";

//------------------------------------------------------------------------------

TcpIoDaemon::TcpIoDaemon(const TcpIpService* service, ipport_t port) :
   Daemon(MakeName(port).c_str(), 1),
   service_(service),
   port_(port)
{
   Debug::ft("TcpIoDaemon.ctor");
}

//------------------------------------------------------------------------------

TcpIoDaemon::~TcpIoDaemon()
{
   Debug::ftnt("TcpIoDaemon.dtor");
}

//------------------------------------------------------------------------------

Thread* TcpIoDaemon::CreateThread()
{
   Debug::ft("TcpIoDaemon.CreateThread");

   return new TcpIoThread(this, service_, port_);
}

//------------------------------------------------------------------------------

void TcpIoDaemon::Display(ostream& stream,
   const string& prefix, const Flags& options) const
{
   Daemon::Display(stream, prefix, options);

   stream << prefix << "service : " << strObj(service_) << CRLF;
   stream << prefix << "port    : " << port_ << CRLF;
}

//------------------------------------------------------------------------------

TcpIoDaemon* TcpIoDaemon::GetDaemon(const TcpIpService* service, ipport_t port)
{
   Debug::ft("TcpIoDaemon.GetDaemon");

   auto reg = Singleton< DaemonRegistry >::Instance();
   auto name = MakeName(port);
   auto daemon = static_cast< TcpIoDaemon* >(reg->FindDaemon(name.c_str()));

   if(daemon != nullptr) return daemon;
   return new TcpIoDaemon(service, port);
}

//------------------------------------------------------------------------------

string TcpIoDaemon::MakeName(ipport_t port)
{
   Debug::ft("TcpIoDaemon.MakeName");

   //  A Daemon requires a unique name, so append the port number
   //  to the basic name.
   //
   string name(TcpIoDaemonName);
   name.push_back('_');
   name.append(std::to_string(port));
   return name;
}

//------------------------------------------------------------------------------

void TcpIoDaemon::Patch(sel_t selector, void* arguments)
{
   Daemon::Patch(selector, arguments);
}

//==============================================================================

fixed_string UdpIoDaemonName = "udp";

//------------------------------------------------------------------------------

UdpIoDaemon::UdpIoDaemon(const UdpIpService* service, ipport_t port) :
   Daemon(MakeName(port).c_str(), 1),
   service_(service),
   port_(port)
{
   Debug::ft("UdpIoDaemon.ctor");
}

//------------------------------------------------------------------------------

UdpIoDaemon::~UdpIoDaemon()
{
   Debug::ftnt("UdpIoDaemon.dtor");
}

//------------------------------------------------------------------------------

Thread* UdpIoDaemon::CreateThread()
{
   Debug::ft("UdpIoDaemon.CreateThread");

   return new UdpIoThread(this, service_, port_);
}

//------------------------------------------------------------------------------

void UdpIoDaemon::Display(ostream& stream,
   const string& prefix, const Flags& options) const
{
   Daemon::Display(stream, prefix, options);

   stream << prefix << "service : " << strObj(service_) << CRLF;
   stream << prefix << "port    : " << port_ << CRLF;
}

//------------------------------------------------------------------------------

UdpIoDaemon* UdpIoDaemon::GetDaemon(const UdpIpService* service, ipport_t port)
{
   Debug::ft("UdpIoDaemon.GetDaemon");

   auto reg = Singleton< DaemonRegistry >::Instance();
   auto name = MakeName(port);
   auto daemon = static_cast< UdpIoDaemon* >(reg->FindDaemon(name.c_str()));

   if(daemon != nullptr) return daemon;
   return new UdpIoDaemon(service, port);
}

//------------------------------------------------------------------------------

string UdpIoDaemon::MakeName(ipport_t port)
{
   Debug::ft("UdpIoDaemon.MakeName");

   //  A Daemon requires a unique name, so append the port number
   //  to the basic name.
   //
   string name(UdpIoDaemonName);
   name.push_back('_');
   name.append(std::to_string(port));
   return name;
}

//------------------------------------------------------------------------------

void UdpIoDaemon::Patch(sel_t selector, void* arguments)
{
   Daemon::Patch(selector, arguments);
}
}
