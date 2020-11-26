// Emacs style mode select   -*- C++ -*- 
//-----------------------------------------------------------------------------
//
// $Id$
//
// Copyright (C) 1998-2006 by Randy Heit (ZDoom).
// Copyright (C) 2006-2020 by The Odamex Team.
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// DESCRIPTION:
//	Error Handling
//
//-----------------------------------------------------------------------------


#ifndef __ERRORS_H__
#define __ERRORS_H__

#include <string>

class CDoomError
{
public:
	CDoomError (std::string message) : m_Message(message) {}
	std::string GetMsg (void) const { return m_Message;	}

private:
	std::string m_Message;
};

class CRecoverableError : public CDoomError
{
public:
	CRecoverableError(std::string message) : CDoomError(message) {}
};

class CFatalError : public CDoomError
{
public:
	CFatalError(std::string message) : CDoomError(message) {}
};

#ifdef _XBOX

// nxdk does not yet support C++ extensions, however it does support SEH

#include <string.h>
#include "win32inc.h"

#define EXCEPTION_ODAMEX_MASK     0xDDEAD0 // last bit indicates nonfatal/fatal
#define EXCEPTION_ODAMEX_NONFATAL EXCEPTION_ODAMEX_MASK
#define EXCEPTION_ODAMEX_FATAL    (EXCEPTION_ODAMEX_MASK | 1)

class CSEHHelper
{
public:
	void Throw(const DWORD type, const char *msg)
	{
		m_Message = msg;
		RaiseException (type, 0, 0, NULL);
	}
	const std::string& GetMsg(void)
	{
		return m_Message;
	}
private:
	std::string m_Message;
};

extern CSEHHelper seh_helper;

#define ETRY() __try

#define ECATCH_FATAL() __except ( \
		GetExceptionCode() == EXCEPTION_ODAMEX_FATAL ? \
		EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH \
	)

#define ECATCH_RECOVERABLE() __except ( \
		GetExceptionCode() == EXCEPTION_ODAMEX_NONFATAL ? \
		EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH \
	)

#define ECATCH_DOOMERROR() __except ( \
		(GetExceptionCode() & EXCEPTION_ODAMEX_MASK) == EXCEPTION_ODAMEX_MASK ? \
		EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH \
	)

#define ECATCH_ANY() __except ( \
		EXCEPTION_EXECUTE_HANDLER \
	)

#define ETHROW_FATAL(msg) seh_helper.Throw(EXCEPTION_ODAMEX_FATAL, msg)

#define ETHROW_RECOVERABLE(msg) seh_helper.Throw(EXCEPTION_ODAMEX_NONFATAL, msg)

#define ETHROW_RETHROW() seh_helper.Throw(EXCEPTION_ODAMEX_FATAL, seh_helper.GetMsg().c_str())

#define GET_EXCEPTION_MSG() (seh_helper.GetMsg())

#else // _XBOX

// use C++ exceptions

#define ETRY() try
#define ECATCH_FATAL() catch (CFatalError &error)
#define ECATCH_RECOVERABLE() catch (CRecoverableError &error)
#define ECATCH_DOOMERROR() catch (CDoomError &error)
#define ECATCH_ANY() catch (...)
#define ETHROW_FATAL(msg) throw CFatalError(msg)
#define ETHROW_RECOVERABLE(msg) throw CRecoverableError(msg)
#define ETHROW_RETHROW() throw
#define GET_EXCEPTION_MSG() error.GetMsg().c_str()

#endif // _XBOX

#endif //__ERRORS_H__

