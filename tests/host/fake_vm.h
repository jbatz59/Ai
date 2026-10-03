#pragma once

#include <string>

struct FakeVmFrame {
  int status = 0;              // the game's own pcall result
  bool stackBalanced = true;   // Chroma's safe point must leave the game's stack untouched
  bool demigod = false;        // set by game.game:GetActivePlayer():SetDemigod(true)
};

std::string FakeVmBindings();   // bindings file for the fake VM (no Game.TickHook: pcall mode)
bool FakeVmInit();
FakeVmFrame FakeVmTick();       // one "game frame": pcall a script function on the main state
