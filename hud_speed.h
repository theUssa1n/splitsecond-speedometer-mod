// ---------------------------------------------------------
// GAME BUILD + NATIVE HUD SPEED
// ---------------------------------------------------------
#pragma once

#include <cstdint>

// Everything this mod touches is a hardcoded address, so the running build has
// to be identified before the first of them is used. Two builds are known: the
// retail / cracked exe and the steam one. They share the module base (0x400000)
// and have almost the same .rdata (steam is shifted by 0x58), but their code and
// globals are laid out differently, so each build carries its own table.
namespace GameBuild {
    enum class Id { Unknown, Retail, Steam };

    // Reads the exe once and remembers the answer. Id::Unknown means the mod
    // must stay completely away from memory.
    Id Detect();

    // Global holding the in-game UI object; zero outside a race.
    uintptr_t InGameUiPtr();

    // Hud::cTargetPlayerElement class vtable, used to locate the element.
    uint32_t ElementVtable();

    // Global holding the array of cars; index 0 is player 1.
    uintptr_t VehicleArrayPtr();
}

namespace HudSpeed {
    // Loads speedo.ini (creating it on first run) and starts the scanner thread
    // that locates the game's HUD element.
    void Init();

    // Hands the current speed of every local player to the game's own HUD
    // element. Called once per rendered frame.
    void Update(bool inRace);

    // Switches the readout between miles and kilometres, and stores the choice
    // in speedo.ini so it survives a restart.
    void ToggleUnit();
}
