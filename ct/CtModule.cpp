//==============================================================================
//
//  CtModule.cpp
//
//  Copyright (C) 2017  Greg Utas
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
#include "CtModule.h"
#include "CodeCoverage.h"
#include "CodeWarning.h"
#include "CtIncrement.h"
#include "CxxExecute.h"
#include "CxxRoot.h"
#include "CxxSymbols.h"
#include "Debug.h"
#include "Lexer.h"
#include "Library.h"
#include "ModuleRegistry.h"
#include "NbModule.h"
#include "NtModule.h"
#include "Singleton.h"
#include "SysTypes.h"

using namespace NodeBase;
using namespace NodeTools;

//------------------------------------------------------------------------------

namespace CodeTools
{
fn_name CtModule_ctor = "CtModule.ctor";

CtModule::CtModule() : Module()
{
   Debug::ft(CtModule_ctor);

   //  Create the modules required by CodeTools.
   //
   Singleton< NbModule >::Instance();
   Singleton< NtModule >::Instance();
   Singleton< ModuleRegistry >::Instance()->BindModule(*this);
}

//------------------------------------------------------------------------------

fn_name CtModule_dtor = "CtModule.dtor";

CtModule::~CtModule()
{
   Debug::ft(CtModule_dtor);
}

//------------------------------------------------------------------------------

fn_name CtModule_Shutdown = "CtModule.Shutdown";

void CtModule::Shutdown(RestartLevel level)
{
   Debug::ft(CtModule_Shutdown);

   auto coverdb = Singleton< CodeCoverage >::Extant();
   if(coverdb != nullptr) coverdb->Shutdown(level);

   Context::Shutdown(level);
   Singleton< CxxRoot >::Instance()->Shutdown(level);
   Singleton< Library >::Instance()->Shutdown(level);
   Singleton< CxxSymbols >::Instance()->Shutdown(level);
}

//------------------------------------------------------------------------------

fn_name CtModule_Startup = "CtModule.Startup";

void CtModule::Startup(RestartLevel level)
{
   Debug::ft(CtModule_Startup);

   CodeWarning::Initialize();
   Lexer::Initialize();

   //  Create/start singletons.
   //
   Singleton< CtIncrement >::Instance()->Startup(level);
   Singleton< CxxSymbols >::Instance()->Startup(level);
   Singleton< Library >::Instance()->Startup(level);
   Singleton< CxxRoot >::Instance()->Startup(level);
   Context::Startup(level);
}
}
