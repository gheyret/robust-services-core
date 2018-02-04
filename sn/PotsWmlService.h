//==============================================================================
//
//  PotsWmlService.h
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
#ifndef POTSWMLSERVICE_H_INCLUDED
#define POTSWMLSERVICE_H_INCLUDED

#include "Initiator.h"
#include "NbTypes.h"
#include "Service.h"

using namespace SessionBase;

//------------------------------------------------------------------------------

namespace PotsBase
{
class PotsWmlInitiator : public Initiator
{
public:
   PotsWmlInitiator();
private:
   virtual EventHandler::Rc ProcessEvent(const ServiceSM& parentSsm,
      Event& currEvent, Event*& nextEvent) const override;
};

class PotsWmlService : public Service
{
   friend class Singleton< PotsWmlService >;
private:
   PotsWmlService();
   ~PotsWmlService();
   virtual ServiceSM* AllocModifier() const override;
};

class PotsWmlActivate : public Service
{
   friend class Singleton< PotsWmlActivate >;
private:
   PotsWmlActivate();
   ~PotsWmlActivate();
   virtual ServiceSM* AllocModifier() const override;
};

class PotsWmlDeactivate : public Service
{
   friend class Singleton< PotsWmlDeactivate >;
private:
   PotsWmlDeactivate();
   ~PotsWmlDeactivate();
   virtual ServiceSM* AllocModifier() const override;
};
}
#endif
