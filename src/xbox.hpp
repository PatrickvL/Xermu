#pragma once
// ---------------------------------------------------------------------------
// xbox.hpp — Xbox physical address map + device stubs (umbrella header).
//
// Individual device headers live in src/xbox/.  This file includes them all
// so that existing consumers (test_runner.cpp, xbe_loader.hpp, main.cpp) can
// simply `#include "xbox.hpp"` and get everything.
// ---------------------------------------------------------------------------

#include "xbox/address_map.hpp"
#include "xbox/nv2a.hpp"
#include "xbox/apu.hpp"
#include "xbox/ide.hpp"
#include "xbox/usb.hpp"
#include "xbox/ioapic.hpp"
#include "xbox/ram_mirror.hpp"
#include "xbox/flash.hpp"
#include "xbox/pci.hpp"
#include "xbox/smbus.hpp"
#include "xbox/pic.hpp"
#include "xbox/pit.hpp"
#include "xbox/misc_io.hpp"
#include "xbox/setup.hpp"
