#pragma once
// Cross-thread work queues.
//  - Render queue: drained at the start of every Present (render thread).
//  - Game queue: drained inside the game-thread tick hook when the "Game.TickHook" binding resolves;
//    otherwise drained right after the render queue (documented fallback).
#include <functional>

namespace cg::tasks {

using Fn = std::function<void()>;

void PostRender(Fn fn);
void PostGame(Fn fn);

void DrainRender();   // overlay only
void DrainGame();     // game tick hook (or overlay fallback)

void SetGameThreadHookActive(bool active);
bool GameThreadHookActive();

// Fire-and-forget background work (bindings resolution, memory scans). Uses a small thread pool.
void RunAsync(Fn fn);
void ShutdownPool();   // joins workers; called during unload

}  // namespace cg::tasks
