#pragma once
#ifndef HT_SERIAL_MSGPACK_H
#define HT_SERIAL_MSGPACK_H

#include <DebugLog.h>
#ifdef MSGPACK_DEBUGLOG_ENABLE
#include <DebugLogEnable.h>
#else
#include <DebugLogDisable.h>
#endif

#include "MsgPack/Types.h"
#include "MsgPack/Packer.h"
#include "MsgPack/Unpacker.h"
#include "MsgPack/Utility.h"

namespace MsgPack = arduino::msgpack;

#include <DebugLogRestoreState.h>

#endif  // ARDUINOMSGPACK_H
