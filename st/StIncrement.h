//==============================================================================
//
//  StIncrement.h
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
#ifndef STINCREMENT_H_INCLUDED
#define STINCREMENT_H_INCLUDED

#include "CliIncrement.h"
#include "NbTypes.h"
#include "NtIncrement.h"

using namespace NodeTools;

//------------------------------------------------------------------------------

namespace SessionTools
{
//  The SIZES command for this increment.
//
class StSizesCommand : public SizesCommand
{
public:
   StSizesCommand() { }
   virtual ~StSizesCommand() { }
protected:
   virtual void DisplaySizes(CliThread& cli, bool all) const override;
private:
   virtual word ProcessCommand(CliThread& cli) const override;
};

//------------------------------------------------------------------------------
//
//  Increment for SessionBase tools and tests.
//
class StIncrement : public CliIncrement
{
   friend class Singleton< StIncrement >;
private:
   //  Private because this singleton is not subclassed.
   //
   StIncrement();

   //  Private because this singleton is not subclassed.
   //
   ~StIncrement();

   //  Overridden to enter the increment.
   //
   virtual void Enter() override;
};
}
#endif
