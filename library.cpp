#include <atomic>
#include <cstdint>
#include <Windows.h>
#include <iostream>
#include <random>
#include <thread>

#include "MinHook/include/MinHook.h"


HMODULE lib_tibiame_base = nullptr;

bool is_In_combat = false;
DWORD g_CombatStartTime = 0;

bool g_MacroEnabled = false;
bool g_KeyPressed = false;
bool g_UseMovementMacro = true;

int g_InitialX = 0;
int g_InitialY = 0;

constexpr int MAX_RADIUS = 4; // Maximum distance from the initial position
constexpr int STEPS_IN_DIRECTION = 3; // Number of steps to take in the same direction before changing

int g_CurrentDirection = 0;
int g_StepsRemaining = 0;
int g_Seed = 0x12345678;

enum Direction : unsigned int {
    Up    = 0,
    Right = 1,
    Down  = 2,
    Left  = 3
};

class TMovementManager;
class TCreature {
public:
    uint8_t getX() {
        // base + 0x8 -> 0x18 -> 0x10 -> 0xD8
        uintptr_t ptr = *reinterpret_cast<uintptr_t *>(reinterpret_cast<uintptr_t>(this) + 0x8);
        ptr = *reinterpret_cast<uintptr_t *>(ptr + 0x18);
        ptr = *reinterpret_cast<uintptr_t *>(ptr + 0x10);
        return *reinterpret_cast<uint8_t *>(ptr + 0xD8);
    }

    uint8_t getY() {
        // base + 0x8 -> 0xC0 -> 0x234
        uintptr_t ptr = *reinterpret_cast<uintptr_t *>(reinterpret_cast<uintptr_t>(this) + 0x8);
        ptr = *reinterpret_cast<uintptr_t *>(ptr + 0xC0);
        return *reinterpret_cast<uint8_t *>(ptr + 0x234);
    }

    int getHp() {
        // base + 0x8 -> 0x18 -> 0x10 -> 0x1D8
        uintptr_t ptr = *reinterpret_cast<uintptr_t *>(reinterpret_cast<uintptr_t>(this) + 0x8);
        ptr = *reinterpret_cast<uintptr_t *>(ptr + 0x18);
        ptr = *reinterpret_cast<uintptr_t *>(ptr + 0x10);
        return *reinterpret_cast<int *>(ptr + 0x1D8);
    }

    int getTotalHp() {
        // base + 0x8 -> 0x18 -> 0x10 -> 0x1DC
        uintptr_t ptr = *reinterpret_cast<uintptr_t *>(reinterpret_cast<uintptr_t>(this) + 0x8);
        ptr = *reinterpret_cast<uintptr_t *>(ptr + 0x18);
        ptr = *reinterpret_cast<uintptr_t *>(ptr + 0x10);
        return *reinterpret_cast<int *>(ptr + 0x1DC);
    }

    // Unicode nick
    wchar_t* getNick() {
        // 0x8 -> 0x18 -> 0x10 -> 0xE8
        uintptr_t ptr = *reinterpret_cast<uintptr_t *>(reinterpret_cast<uintptr_t>(this) + 0x8);
        ptr = *reinterpret_cast<uintptr_t *>(ptr + 0x18);
        ptr = *reinterpret_cast<uintptr_t *>(ptr + 0x10);
        return reinterpret_cast<wchar_t *>(ptr + 0xE8);
    }
};

TMovementManager* g_MovementManager = nullptr;
uintptr_t moveFunAddr = reinterpret_cast<uintptr_t>(nullptr);
typedef __int64(__fastcall* MoveFunc)(TMovementManager*, __int64, unsigned int);

TCreature* g_LocalPlayer = nullptr;

typedef __int64(__fastcall* tMsgInSetNewCreature)(__int64 a1, __int64 a2);
tMsgInSetNewCreature oMsgInSetNewCreature = nullptr;

typedef __int64(__fastcall* tMoveFn)(TMovementManager* tMovementManager, TCreature* creature, unsigned int direction);
tMoveFn originalMove = nullptr;

typedef __int64 (__fastcall* tMsgInSetPosition)(__int64 a1, __int64 a2);
tMsgInSetPosition oMsgInSetPosition = nullptr;

typedef void (__fastcall *tCmdQuitGame)(long long param_1);
tCmdQuitGame oCmdQuitGame = nullptr;

typedef void (__fastcall *tMsgInCreateCharacterInfo)(void* thisPtr);
tMsgInCreateCharacterInfo oMsgInCreateCharacterInfo = nullptr;

typedef void (__fastcall *tApplyAnimation)(void* creature, void* gcObj, uint8_t animId);
tApplyAnimation oApplyAnimation = nullptr;

typedef void(__fastcall* tProcessNewCreatureAnimation)(void* thisPtr, void* creaturePtr, int animationId);
tProcessNewCreatureAnimation oProcessNewCreatureAnimation = nullptr;

typedef char(__fastcall* tKeyEventCore)(__int64 block, int pressed, unsigned int keyCode, unsigned int param1, unsigned int param2);
tKeyEventCore oKeyEventCore = nullptr;
tKeyEventCore SendKeyEvent = nullptr;
__int64 g_Block = 0;

char __fastcall hkKeyEventCore(__int64 block, int pressed, unsigned int keyCode, unsigned int p1, unsigned int p2) {
    if (g_Block == 0) g_Block = block;
    return oKeyEventCore(block, pressed, keyCode, p1, p2);
}

void __fastcall hkProcessNewCreatureAnimation(void* thisPtr, void* creaturePtr, const int animId) {
    is_In_combat = true;
    g_CombatStartTime = GetTickCount();
    // oProcessNewCreatureAnimation(thisPtr, creaturePtr, animId);
}

__int64 __fastcall hookMove(TMovementManager* tMovementManager, TCreature* creature, unsigned int direction) {
    if (g_MovementManager == nullptr && g_LocalPlayer == nullptr) {
        g_MovementManager = tMovementManager;
        g_LocalPlayer = creature;
        printf("[*] MovementManager instance stored: %p\n", g_MovementManager);
        printf("[*] LocalPlayer instance stored: %p\n", g_LocalPlayer);
        MH_DisableHook(reinterpret_cast<void*>(moveFunAddr));
    }
    if (g_LocalPlayer != nullptr) {
        const int playerHp = g_LocalPlayer->getHp();
        const int playerTotalHp = g_LocalPlayer->getTotalHp();
        const int playerX = g_LocalPlayer->getX();
        const int playerY = g_LocalPlayer->getY();
        printf("Player HP: %d/%d\n", playerHp, playerTotalHp);
        printf("Player Position: (%d, %d)\n", playerX, playerY);
    }
    return originalMove(tMovementManager, creature, direction);
}

__int64 __fastcall hkMsgInSetNewCreature(__int64 a1, __int64 a2) {
    const uint8_t creatureId = *reinterpret_cast<uint8_t *>(a2 + 0x20);  // +32
    const uint8_t posX = *reinterpret_cast<uint8_t *>(a2 + 0x30);        // +48
    const uint8_t posY = *reinterpret_cast<uint8_t *>(a2 + 0x31);        // +49

    printf("[*] Creature ID: %u, Position: (%u, %u)\n", creatureId, posX, posY);
    return oMsgInSetNewCreature(a1, a2);
}

void resetMacroState() {
    is_In_combat = false;
    g_MacroEnabled = false;
    g_KeyPressed = false;
    g_UseMovementMacro = true;

    g_InitialX = 0;
    g_InitialY = 0;

    g_CurrentDirection = 0;
    g_StepsRemaining = 0;

    g_MovementManager = nullptr;
    g_LocalPlayer = nullptr;

    printf("[*] Macro state reset.\n");
}

void __fastcall hkCmdQuitGame(const long long param_1)
{
    printf("[*] cmdQuitGame intercepted. %lld", param_1);
    resetMacroState();
    oCmdQuitGame(param_1);
}

[[noreturn]] DWORD WINAPI keyPressControllerThread(LPVOID) {
    while (true) {
        if (is_In_combat && g_LocalPlayer != nullptr) {
            // Use slot 1 -> Book
            SendKeyEvent(g_Block, 1, 0x32, 50, 1); // Down 2
            SendKeyEvent(g_Block, 1, 0x37, 55, 1); // Down 7
            Sleep(100);
            SendKeyEvent(g_Block, 0, 0x37, 55, 1); // Up 7
            SendKeyEvent(g_Block, 0, 0x32, 50, 1); // Up 2
            //

            if (static_cast<float>(g_LocalPlayer->getHp()) / static_cast<float>(g_LocalPlayer->getTotalHp()) <= 0.5f) {
                printf("[*] HP is below 50%%, starting potion usage...\n");

                // If HP is still below 50%, use potions
                if (static_cast<float>(g_LocalPlayer->getHp()) / static_cast<float>(g_LocalPlayer->getTotalHp()) <= 0.5f) {
                    // Attempts use potions 1-9
                    for (int i = 1; i <= 9; ++i) {
                        const int vk_code = 0x30 + i;
                        const int scan_code = 48 + i;

                        // Enable sloth mode
                        SendKeyEvent(g_Block, 1, 0x32, 50, 1); // Down 2
                        SendKeyEvent(g_Block, 1, vk_code, scan_code, 1); // Down 1-9
                        Sleep(100);
                        SendKeyEvent(g_Block, 0, vk_code, scan_code, 1); // Up 1-9
                        SendKeyEvent(g_Block, 0, 0x32, 50, 1); // Up 2
                        Sleep(1500); // Wait for the potion to take effect

                        // Check if HP is still below 50% after each potion
                        if (static_cast<float>(g_LocalPlayer->getHp()) / static_cast<float>(g_LocalPlayer->getTotalHp()) > 0.5f) {
                            printf("[*] HP restored above 50%%, stopping potion usage.\n");
                            break;
                        }
                    }

                    // If HP is still below 10% after all potions quit the game
                    if (static_cast<float>(g_LocalPlayer->getHp()) / static_cast<float>(g_LocalPlayer->getTotalHp()) <= 0.1f) {exit(0);}
                }
            }
        }

        if (g_MacroEnabled) {
            // Use Skill -> Q, E
            SendKeyEvent(g_Block, 1, 0x51, 113, 1); // Q down
            SendKeyEvent(g_Block, 0, 0x51, 113, 1); // Q up
            Sleep(100);
            SendKeyEvent(g_Block, 1, 0x45, 101, 1); // E down
            SendKeyEvent(g_Block, 0, 0x45, 101, 1); // E up
            Sleep(1200); // Wait for the skills to take effect
        } else {
            Sleep(500);
        }
    }
}

int randomDirection() {
    g_Seed = 1664525 * g_Seed + 1013904223;
    return (g_Seed >> 24) & 0x03;
}

bool isWithinRadius(const int x, const int y) {
    const int dx = x - g_InitialX;
    const int dy = y - g_InitialY;
    return dx * dx + dy * dy <= MAX_RADIUS * MAX_RADIUS;
}

bool wouldStayWithinRadius(const int dir, int x, int y) {
    switch (dir) {
        case Up:    y -= 1; break;
        case Right: x += 1; break;
        case Down:  y += 1; break;
        case Left:  x -= 1; break;
        default:;
    }
    return isWithinRadius(x, y);
}

int getSmartDirection() {
    if (!g_LocalPlayer) return 0;

    const int posX = g_LocalPlayer->getX();
    const int posY = g_LocalPlayer->getY();

    if (g_StepsRemaining <= 0 || !wouldStayWithinRadius(g_CurrentDirection, posX, posY)) {
        for (int i = 0; i < 10; ++i) {
            if (const int dir = randomDirection(); wouldStayWithinRadius(dir, posX, posY)) {
                g_CurrentDirection = dir;
                g_StepsRemaining = STEPS_IN_DIRECTION;
                break;
            }
        }
    }

    g_StepsRemaining--;
    return g_CurrentDirection;
}

[[noreturn]] void movementMacroThread() {
    int stepCounter = 0;
    while (true) {
        if (is_In_combat && GetTickCount() - g_CombatStartTime >= 5000) {
            is_In_combat = false;
            printf("[*] Combat ended, resuming macro.\n");
        }

        if (g_MacroEnabled && originalMove && g_MovementManager && g_LocalPlayer && !is_In_combat && g_UseMovementMacro) {
            const int dir = getSmartDirection();
            originalMove(g_MovementManager, g_LocalPlayer, dir);
            printf("[*] Moving in smart direction: %d\n", dir);

            stepCounter++;
            if (stepCounter >= 4) {
                printf("[*] Pausing for 5 seconds after 4 steps.\n");
                Sleep(5000);
                stepCounter = 0;
            } else {
                Sleep(400);
            }
        } else {
            Sleep(100);
        }
    }
}


[[noreturn]] DWORD WINAPI keyboardListenerThread(LPVOID) {
    while (true) {
        if (GetAsyncKeyState(VK_F1) & 0x8000) {
            if (!g_KeyPressed) {
                g_MacroEnabled = !g_MacroEnabled;
                printf("[*] Macro state: %s\n", g_MacroEnabled ? "Enabled" : "Disabled");
                g_KeyPressed = true;

                if (g_LocalPlayer == nullptr) {
                    SendKeyEvent(g_Block, 1, 0x57, 119, 1); // W down
                    Sleep(100);
                    SendKeyEvent(g_Block, 0, 0x57, 119, 1); // W up
                }

                if (g_MacroEnabled && g_LocalPlayer != nullptr) {
                    g_InitialX = g_LocalPlayer->getX();
                    g_InitialY = g_LocalPlayer->getY();

                    printf("[*][%ls] Initial position set: (%d, %d) HP: %d/%d\n", g_LocalPlayer->getNick(),
                        g_InitialX, g_InitialY, g_LocalPlayer->getTotalHp(), g_LocalPlayer->getTotalHp());
                }
            }
        } else if (GetAsyncKeyState(VK_F2) & 0x8000) {
            if (!g_KeyPressed) {
                g_UseMovementMacro = !g_UseMovementMacro;
                printf("[*] Movement macro state: %s\n", g_UseMovementMacro ? "Enabled" : "Disabled");
                g_KeyPressed = true;
            }
        } else {
            g_KeyPressed = false;
        }
        Sleep(100);
    }
}


void startMacro() {
    CreateThread(nullptr, 0, keyboardListenerThread, nullptr, 0, nullptr);
    CreateThread(nullptr, 0, keyPressControllerThread, nullptr, 0, nullptr);
    CreateThread(nullptr, 0, reinterpret_cast<LPTHREAD_START_ROUTINE>(movementMacroThread), nullptr, 0, nullptr);
}


DWORD WINAPI MainThread(LPVOID) {
    AllocConsole();

    freopen("CONOUT$", "w", stdout);
    SetConsoleTitleA("Protons Bot");

    printf("[*] Awaiting libtibiame_x64.dll...\n");
    while (!lib_tibiame_base) {
        lib_tibiame_base = GetModuleHandleA("libtibiame_x64.dll");
        Sleep(100);
    }

    printf("[*] libtibiame_x64.dll loaded: %p\n", lib_tibiame_base);

    moveFunAddr = reinterpret_cast<uintptr_t>(lib_tibiame_base) + 0x80F20;
    const uintptr_t cmdQuitGameAddr = reinterpret_cast<uintptr_t>(lib_tibiame_base) + 0xC4F90;
    const uintptr_t processNewCreatureAnimation = reinterpret_cast<uintptr_t>(lib_tibiame_base) + 0x149900;
    const uintptr_t keyEventAddr = reinterpret_cast<uintptr_t>(lib_tibiame_base) + 0xF470;
    SendKeyEvent = reinterpret_cast<tKeyEventCore>(keyEventAddr);

    // Initialize MinHook
    if (MH_Initialize() != MH_OK) {
        printf("[!] Failed to initialize MinHook.\n");
        return 0;
    }

    // Hook Move function
    MH_CreateHook(reinterpret_cast<void*>(moveFunAddr),
                   reinterpret_cast<void*>(&hookMove),
                   reinterpret_cast<void**>(&originalMove));

    // Hook SubOnQuitGame
    MH_CreateHook(reinterpret_cast<void*>(cmdQuitGameAddr),
                   reinterpret_cast<void*>(&hkCmdQuitGame),
                   reinterpret_cast<void**>(&oCmdQuitGame));

    // Hook MsgInSetNewCreature
    MH_CreateHook(reinterpret_cast<void*>(processNewCreatureAnimation),
                   reinterpret_cast<void*>(&hkProcessNewCreatureAnimation),
                   reinterpret_cast<void**>(&oProcessNewCreatureAnimation));

    // Hook KeyEvent
    MH_CreateHook(reinterpret_cast<void*>(keyEventAddr),
                   reinterpret_cast<void*>(&hkKeyEventCore),
                   reinterpret_cast<void**>(&oKeyEventCore));

    // Enable Move hook
    MH_EnableHook(reinterpret_cast<void*>(moveFunAddr));

    // Enable cmdQuitGame
    MH_EnableHook(reinterpret_cast<void*>(cmdQuitGameAddr));

    // Enable MsgInSetNewCreature
    MH_EnableHook(reinterpret_cast<void*>(processNewCreatureAnimation));

    // Enable KeyEvent
    MH_EnableHook(reinterpret_cast<void*>(keyEventAddr));

    // Start Macro
    startMacro();
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, const DWORD ul_reason_for_call, LPVOID lpReserved) {
    if (ul_reason_for_call == DLL_PROCESS_ATTACH)
        CreateThread(nullptr, 0, MainThread, nullptr, 0, nullptr);

    return TRUE;
}
