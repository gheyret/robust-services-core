//==============================================================================
//
//  CxxRoot.cpp
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
#include "CxxRoot.h"
#include "CxxDirective.h"
#include "Tool.h"
#include <bitset>
#include <cstdio>
#include <istream>
#include <ostream>
#include <string>
#include <utility>
#include "CodeFile.h"
#include "CodeTypes.h"
#include "Cxx.h"
#include "CxxExecute.h"
#include "CxxString.h"
#include "CxxStrLiteral.h"
#include "CxxVector.h"
#include "Debug.h"
#include "Parser.h"
#include "Restart.h"
#include "Singleton.h"
#include "SysTime.h"
#include "SysTypes.h"
#include "ToolTypes.h"

using namespace NodeBase;
using std::ostream;
using std::string;

//------------------------------------------------------------------------------

namespace CodeTools
{
//  Built-in macros: __DATE__, __FILE__, __func__, __TIME__, and __LINE__.
//
typedef std::unique_ptr< StrLiteral > StrLiteralPtr;
typedef std::vector< StrLiteralPtr > StrLiteralPtrVector;

class MacroDATE : public Macro
{
public:
   MacroDATE();
   CxxToken* GetValue() const override;
   CxxScoped* Referent() const override { return StrLiteral::GetReferent(); }
private:
   mutable StrLiteralPtr unknown_;
   mutable StrLiteralPtr date_;
};

MacroDATE::MacroDATE() : Macro(string("__DATE__"))
{
   Debug::ft("MacroDATE.ctor");
}

CxxToken* MacroDATE::GetValue() const
{
   Debug::ft("MacroDATE.GetValue");

   if(date_ != nullptr) return date_.get();

   auto time = Parser::GetTime();

   if(time != nullptr)
   {
      date_ = StrLiteralPtr(new StrLiteral(time->to_str(SysTime::HighAlpha)));
      return date_.get();
   }

   if(unknown_ != nullptr) return unknown_.get();
   unknown_ = StrLiteralPtr(new StrLiteral("??-???-????"));
   return unknown_.get();
}

//------------------------------------------------------------------------------

class MacroFILE : public Macro
{
public:
   MacroFILE();
   CxxToken* GetValue() const override;
   CxxScoped* Referent() const override { return StrLiteral::GetReferent(); }
private:
   mutable StrLiteralPtr unknown_;
   mutable StrLiteralPtrVector files_;
};

MacroFILE::MacroFILE() : Macro(string("__FILE__"))
{
   Debug::ft("MacroFILE.ctor");
}

CxxToken* MacroFILE::GetValue() const
{
   Debug::ft("MacroFILE.GetValue");

   auto file = Context::File();

   if(file == nullptr)
   {
      if(unknown_ != nullptr) return unknown_.get();
      unknown_ = StrLiteralPtr(new StrLiteral("unknown file"));
      return unknown_.get();
   }

   auto fn = file->Path();

   if(!files_.empty())
   {
      if(files_.back()->GetStr() == fn) return files_.back().get();
   }

   StrLiteralPtr name(new StrLiteral(fn));
   files_.push_back(std::move(name));
   return files_.back().get();
}

//------------------------------------------------------------------------------

class MacroFunc : public Macro
{
public:
   MacroFunc();
   CxxToken* GetValue() const override;
   CxxScoped* Referent() const override { return StrLiteral::GetReferent(); }
private:
   mutable StrLiteralPtr unknown_;
   mutable StrLiteralPtrVector funcs_;
};

MacroFunc::MacroFunc() : Macro(string("__func__"))
{
   Debug::ft("MacroFunc.ctor");
}

CxxToken* MacroFunc::GetValue() const
{
   Debug::ft("MacroFunc.GetValue");

   auto scope = Context::Scope();

   if((scope == nullptr) || (scope->Type() != Cxx::Function))
   {
      if(unknown_ != nullptr) return unknown_.get();
      unknown_ = StrLiteralPtr(new StrLiteral("unknown function"));
      return unknown_.get();
   }

   auto fn = scope->ScopedName(true);

   if(!funcs_.empty())
   {
      if(funcs_.back()->GetStr() == fn) return funcs_.back().get();
   }

   StrLiteralPtr name(new StrLiteral(fn));
   funcs_.push_back(std::move(name));
   return funcs_.back().get();
}

//------------------------------------------------------------------------------

class MacroLINE : public Macro
{
public:
   MacroLINE();
   CxxToken* GetValue() const override;
   CxxScoped* Referent() const override { return StrLiteral::GetReferent(); }
private:
   mutable StrLiteralPtr unknown_;
   mutable StrLiteralPtrVector lines_;
};

MacroLINE::MacroLINE() : Macro(string("__LINE__"))
{
   Debug::ft("MacroLINE.ctor");
}

CxxToken* MacroLINE::GetValue() const
{
   Debug::ft("MacroLINE.GetValue");

   auto parser = Context::GetParser();

   if(parser == nullptr)
   {
      if(unknown_ != nullptr) return unknown_.get();
      unknown_ = StrLiteralPtr(new StrLiteral("unknown line"));
      return unknown_.get();
   }

   StrLiteralPtr line(new StrLiteral(parser->GetLINE()));
   lines_.push_back(std::move(line));
   return lines_.back().get();
}

//------------------------------------------------------------------------------

class MacroTIME : public Macro
{
public:
   MacroTIME();
   CxxToken* GetValue() const override;
   CxxScoped* Referent() const override { return StrLiteral::GetReferent(); }
private:
   mutable StrLiteralPtr unknown_;
   mutable StrLiteralPtr time_;
};

MacroTIME::MacroTIME() : Macro(string("__TIME__"))
{
   Debug::ft("MacroTIME.ctor");
}

CxxToken* MacroTIME::GetValue() const
{
   Debug::ft("MacroTIME.GetValue");

   if(time_ != nullptr) return time_.get();

   auto time = Parser::GetTime();

   if(time != nullptr)
   {
      time_ = StrLiteralPtr(new StrLiteral(time->to_str(SysTime::LowAlpha)));
      return time_.get();
   }

   if(unknown_ != nullptr) return unknown_.get();
   unknown_ = StrLiteralPtr(new StrLiteral("??:??:??"));
   return unknown_.get();
}

//==============================================================================

fixed_string ParserTraceToolName = "ParserTracer";
fixed_string ParserTraceToolExpl = "traces parser's \"code generation\"";

class ParserTraceTool : public Tool
{
   friend class Singleton< ParserTraceTool >;

   ParserTraceTool() : Tool(ParserTracer, 'p', false) { }
   ~ParserTraceTool() = default;
   c_string Expl() const override { return ParserTraceToolExpl; }
   c_string Name() const override { return ParserTraceToolName; }
};

//==============================================================================

CxxRoot::CxxRoot() :
   auto_(nullptr),
   bool_(nullptr),
   char_(nullptr),
   double_(nullptr),
   float_(nullptr),
   int_(nullptr),
   long_(nullptr),
   long_double_(nullptr),
   long_long_(nullptr),
   nullptr_(nullptr),
   nullptr_t_(nullptr),
   short_(nullptr),
   uchar_(nullptr),
   uint_(nullptr),
   ulong_(nullptr),
   ulong_long_(nullptr),
   ushort_(nullptr),
   void_(nullptr)
{
   Debug::ft("CxxRoot.ctor");
}

//------------------------------------------------------------------------------

CxxRoot::~CxxRoot()
{
   Debug::ftnt("CxxRoot.dtor");
}

//------------------------------------------------------------------------------

bool CxxRoot::AddMacro(MacroPtr& macro)
{
   Debug::ft("CxxRoot.AddMacro");

   macro->EnterScope();
   macros_.push_back(std::move(macro));
   return true;
}

//------------------------------------------------------------------------------

void CxxRoot::DefineSymbols(std::istream& stream)
{
   Debug::ft("CxxRoot.DefineSymbols");

   //  Read the symbols from STREAM that are to be #defined for the compile.
   //
   string input;

   while(stream.peek() != EOF)
   {
      std::getline(stream, input);

      if(IsValidIdentifier(input))
      {
         ExprPtr exp(nullptr);
         DefinePtr def(new Define(input, exp));
         macros_.push_back(std::move(def));
      }
   }
}

//------------------------------------------------------------------------------

void CxxRoot::Display(ostream& stream,
   const string& prefix, const Flags& options) const
{
   auto nonqual = options;
   nonqual.reset(DispFQ);

   SortAndDisplayItemPtrs(macros_, stream, EMPTY_STR, nonqual, SortByName);
}

//------------------------------------------------------------------------------

void CxxRoot::Shrink() const
{
   for(auto m = macros_.cbegin(); m != macros_.cend(); ++m)
   {
      (*m)->Shrink();
   }
}

//------------------------------------------------------------------------------

void CxxRoot::Shutdown(RestartLevel level)
{
   Debug::ft("CxxRoot.Shutdown");

   Restart::Release(gns_);
}

//------------------------------------------------------------------------------

void CxxRoot::Startup(RestartLevel level)
{
   Debug::ft("CxxRoot.Startup");

   //  Parser output is now preserved during restarts.
   //
   if(gns_ != nullptr) return;

   CxxChar::Initialize();

   //  Create the global namespace and terminal types.
   //
   gns_.reset(new Namespace(EMPTY_STR, nullptr));

   auto_.reset(new Terminal(AUTO_STR));

   bool_.reset(new Terminal(BOOL_STR));
   bool_->SetNumeric(Numeric::Bool);

   char_.reset(new Terminal(CHAR_STR));
   char_->SetNumeric(Numeric::Char);

   char16_.reset(new Terminal(CHAR16_STR));
   char16_->SetNumeric(Numeric::Char16);

   char32_.reset(new Terminal(CHAR32_STR));
   char32_->SetNumeric(Numeric::Char32);

   double_.reset(new Terminal(DOUBLE_STR));
   double_->SetNumeric(Numeric::Double);

   float_.reset(new Terminal(FLOAT_STR));
   float_->SetNumeric(Numeric::Float);

   int_.reset(new Terminal(INT_STR));
   int_->SetNumeric(Numeric::Int);

   long_.reset(new Terminal(LONG_STR));
   long_->SetNumeric(Numeric::Long);

   long_double_.reset(new Terminal("long double"));
   long_double_->SetNumeric(Numeric::LongDouble);

   long_long_.reset(new Terminal("long long"));
   long_long_->SetNumeric(Numeric::LongLong);

   nullptr_.reset(new Terminal(NULLPTR_STR, NULLPTR_T_STR));

   nullptr_t_.reset(new Terminal(NULLPTR_T_STR));

   short_.reset(new Terminal(SHORT_STR));
   short_->SetNumeric(Numeric::Short);

   uchar_.reset(new Terminal("unsigned char"));
   uchar_->SetNumeric(Numeric::uChar);

   uint_.reset(new Terminal("unsigned int"));
   uint_->SetNumeric(Numeric::uInt);

   ulong_.reset(new Terminal("unsigned long"));
   ulong_->SetNumeric(Numeric::uLong);

   ulong_long_.reset(new Terminal("unsigned long long"));
   ulong_long_->SetNumeric(Numeric::uLongLong);

   ushort_.reset(new Terminal("unsigned short"));
   ushort_->SetNumeric(Numeric::uShort);

   void_.reset(new Terminal(VOID_STR));

   wchar_.reset(new Terminal(WCHAR_STR));
   wchar_->SetNumeric(Numeric::wChar);

   //  Define standard macros.
   //
   MacroPtr macro(new MacroDATE);
   macros_.push_back(std::move(macro));
   macro = MacroPtr(new MacroFILE);
   macros_.push_back(std::move(macro));
   macro = MacroPtr(new MacroFunc);
   macros_.push_back(std::move(macro));
   macro = MacroPtr(new MacroLINE);
   macros_.push_back(std::move(macro));
   macro = MacroPtr(new MacroTIME);
   macros_.push_back(std::move(macro));

   //  #define CT_COMPILER for subs/cstddef.
   //
   ExprPtr exp(nullptr);
   string str("CT_COMPILER");
   DefinePtr def(new Define(str, exp));
   macros_.push_back(std::move(def));

   //  Create the parser trace tool.
   //
   Singleton< ParserTraceTool >::Instance();
}
}
