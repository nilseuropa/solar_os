/*
 * Copyright (c) 2026 Chad Attermann
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 */

#pragma once

#if defined(USTORE_USE_UNIVERSALFS)

#include "../File.h"
#include "../FileSystem.h"

#if defined(ARDUINO_ARCH_NRF52) || defined(ARDUINO_NRF52_ADAFRUIT)

#ifndef USTORE_USE_INTERNALFS
#define USTORE_USE_INTERNALFS
#endif
#include "InternalFSFileSystem.h"
namespace microStore { namespace Adapters {
using UniversalFileSystem = InternalFSFileSystem;
} }

#else

#ifndef USTORE_USE_POSIXFS
#define USTORE_USE_POSIXFS
#endif
#include "PosixFileSystem.h"
namespace microStore { namespace Adapters {
using UniversalFileSystem = PosixFileSystem;
} }

#endif

#endif
