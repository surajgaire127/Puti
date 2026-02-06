#include <dirent.h>
#include "Server.h"
#include "Includes/Encrypt/oxorany_include.h"
#include <android/log.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <math.h>
#include <atomic>
#include <thread>
#include <stdarg.h>
#include <fstream>


Quaternion GetRotationToTheLocation(Vector3 Target, float Height, Vector3 MyEnemy) {
    return Quaternion::LookRotation((Target + Vector3(0, Height, 0)) - MyEnemy, Vector3(0, 1, 0));
}


// Global to prevent log spamming
uintptr_t last_failed_ptr = 0;

uintptr_t currentBestTargetObj = 0;
std::atomic<bool> keepAiming{false};
std::thread aimThread;
Vector3 currentBestTargetPos = Vector3::Zero();

// Add speed hack toggle variable
bool speedHackEnabled = false;

void getUTF8(char* dst, uintptr_t addr) {
    if (addr < 0x10000000) {
        dst[0] = '\0';
        return;
    }

    int stringLen = Read<int>(addr + 0x10);
    if (stringLen <= 0 || stringLen > 64) {
        stringLen = 32; 
    }

    unsigned char raw[128]; 
    for (int i = 0; i < (stringLen * 2); i++) {
        raw[i] = Read<unsigned char>(addr + 0x14 + i);
    }

    int j = 0;
    for (int i = 0; i < (stringLen * 2); i += 2) {
        unsigned short unicode = raw[i] | (raw[i + 1] << 8);
        if (unicode == 0) break;
        if (unicode < 0x80) {
            dst[j++] = (char)unicode;
        } else if (unicode < 0x800) {
            dst[j++] = (char)((unicode >> 6) | 0xC0);
            dst[j++] = (char)((unicode & 0x3F) | 0x80);
        } else if (unicode >= 0xD800 && unicode <= 0xDBFF) { 
            i += 2;
            unsigned short low = raw[i] | (raw[i + 1] << 8);
            unsigned int codepoint = 0x10000 + ((unicode - 0xD800) << 10) + (low - 0xDC00);
            dst[j++] = (char)((codepoint >> 18) | 0xF0);
            dst[j++] = (char)(((codepoint >> 12) & 0x3F) | 0x80);
            dst[j++] = (char)(((codepoint >> 6) & 0x3F) | 0x80);
            dst[j++] = (char)((codepoint & 0x3F) | 0x80);
        } else {
            dst[j++] = (char)((unicode >> 12) | 0xE0);
            dst[j++] = (char)(((unicode >> 6) & 0x3F) | 0x80);
            dst[j++] = (char)((unicode & 0x3F) | 0x80);
        }
        if (j >= 60) break; 
    }
    dst[j] = '\0';
}

bool isWithinFOV(Vector3 screenPos, float fovRadius) {
    float screenCenterX = g_screenWidth / 2.0f;
    float screenCenterY = g_screenHeight / 2.0f;
    float deltaX = screenPos.X - screenCenterX;
    float deltaY = screenPos.Y - screenCenterY;
    float distanceFromCenter = sqrt(deltaX * deltaX + deltaY * deltaY);
    return distanceFromCenter <= fovRadius;
}

void ContinuousAim(uintptr_t oneself) {
    while (keepAiming) {
        uint64_t aimingInfo = Read<uint64_t>(oneself + 0xd20);
        if (aimingInfo) {
            Vector3 startPos = Read<Vector3>(aimingInfo + 0x4c); // old
            Vector3 dir = currentBestTargetPos - startPos;  
            Write<Vector3>(aimingInfo + 0x40, dir);
        }
    }  
}

float Map(float value, float inMin, float inMax, float outMin, float outMax) {
    return outMin + (value - inMin) * (outMax - outMin) / (inMax - inMin);
}

void CreateDataList(Response& SendResponse) {
    if (Actived.activar) {
        float bestDistance = 99999;
        bool hasValidTarget = false;
        Vector3 bestTargetPos = Vector3::Zero();
        uintptr_t bestTargetObj = 0;
        SendResponse.PlayerCount = 0;
        
        uintptr_t gameFacadeOffset = 0xC2132B8; 
        auto GameFacade_c = Read<uintptr_t>(libAddress + gameFacadeOffset);
        
        if (GameFacade_c) {
            auto P2 = Read<uintptr_t>(GameFacade_c + 0xB8);
            if(P2) {
                auto BaseGame = Read<uintptr_t>(P2);
                if (BaseGame) {
                    auto m_Match = Read<uintptr_t>(BaseGame + 0x90);
                    if (m_Match) {
                        auto TimeSerive = Read<uintptr_t>(BaseGame + 0x20);
                        if (TimeSerive) {
                            // Only apply speed hack if enabled
                            if (speedHackEnabled) {
                                Write<float>(TimeSerive + 0x2C, 0.05900000036f); // 5x speed
                            } else {
                                Write<float>(TimeSerive + 0x2C, 0.03299999982f); // Normal speed
                            }
                            
                            auto MatchIsRunning = Read<int>(m_Match + 0xA4);
                            if(MatchIsRunning != 4 && MatchIsRunning != 0 && MatchIsRunning == 1) {
                                auto localPlayer = Read<uintptr_t>(m_Match + 0xb0);
                                if (localPlayer != 0) {
                                    auto FollowCamera = Read<uintptr_t>(localPlayer + 0x5b0);
                                    if (FollowCamera != 0) {
                                        auto Camera = Read<uintptr_t>(FollowCamera + 0x28);
                                        if (Camera != 0) {
                                            auto IntPtrCam = Read<uintptr_t>(Camera + 0x10);
                                            if (IntPtrCam != 0) {
                                                uintptr_t matrixOffset = string2Offset(WRAPPER_MARCO("0xD8"));
                                                auto matrix = Read<D3DMatrix>(IntPtrCam + matrixOffset);
                                                auto dictionary = Read<MonoDictionary *>(BaseGame + 0xc0);
                                                
                                                if (dictionary) {
                                                    int numValues = dictionary->getNumValues();
                                                    auto LocalPosition = GetNodePosition(GetPlayerHeadTF(localPlayer));
                                                    
                                                     auto mIsFiring = Read<int>(localPlayer + 0x758);
                                                    
                                                    for (int x = 0; x < numValues; x++) {
                                                        auto enemyList = Read<uintptr_t>(dictionary->getValues() + 0x8 * x);
                                                        if (enemyList != 0) {
                                                            auto isClientBot = Read<bool>(enemyList + 0x3d8);
                                                            if (isClientBot == 1 || isClientBot == 0) {
                                                                auto AvatarManager = Read<uintptr_t>(enemyList + 0x690);
                                                                if (AvatarManager != 0 && enemyList != localPlayer) {
                                                                    auto UmaAvatarSimple = Read<uintptr_t>(AvatarManager + 0x118);
                                                                    if (UmaAvatarSimple != 0) {
                                                                        auto IsVisible = Read<bool>(UmaAvatarSimple + 0xD8);
                                                                        if (IsVisible == 1) {
                                                                            auto UmaData = Read<uintptr_t>(UmaAvatarSimple + 0x20);
                                                                            if (UmaData != 0) {
                                                                                auto HHCBNAPCKHF = Read<uintptr_t>(enemyList + 0x1ee0);
                                                                                if (HHCBNAPCKHF != 0) {
                                                                                    bool isDieing = false;
                                                                                    auto maybeDead = Read<uintptr_t>(HHCBNAPCKHF + 0x18);
                                                                                    if (maybeDead && Read<int>(maybeDead + 0x10) == 8) isDieing = true;
                                                                                    
                                                                                    if (Read<bool>(UmaData + 0x79) != 0) continue;
                                                                                    if (Read<bool>(enemyList + 0x74) != 0) continue;
                                                                                    
                                                                                    int Health = 0;
                                                                                    auto GetCurHP = Read<uintptr_t>(enemyList + 0x68);
                                                                                    if (GetCurHP != 0) {
                                                                                        auto GetCur = Read<uintptr_t>(GetCurHP + 0x10);
                                                                                        if (GetCur != 0) {
                                                                                            auto HP = Read<uintptr_t>(GetCur + 0x20);
                                                                                            if (HP != 0) Health = (int)Read<short>(HP + 0x18);
                                                                                        }
                                                                                    }
                                                                                    
                                                                                    auto HedColider = Read<uintptr_t>(enemyList + 0x658);
                                                                                    if (pPlayer.aimbotlock) Write(enemyList + 0x78, HedColider);
                                                                                    
                                                                                    auto weapon = Read<uintptr_t>(localPlayer + 0x4c8);
                                                                                    if (pPlayer.aimbotlock && weapon != 0) {
                                                                                        float newAimLock = 0.0f;
                                                                                        Write(weapon + 0x4dc, newAimLock);
                                                                                    }
                                                                                    
                                                                                    
                                                           /*                         if(pPlayer.aimbot && screenDist < pPlayer.AimFov && mIsFiring > 0) {  
                                                                                        Quaternion desiredRot = GetRotationToTheLocation(EnemyPosition, 0.0f, CameraPosition);  
                                                                                        Write<Quaternion>(localPlayer + 0x544, desiredRot);  
                                                                                    }
                                                                            */        
                                                                                    
                                                                                    auto EnemyPosition = GetNodePosition(GetPlayerHeadTF(enemyList));
                                                                                    auto EnemyPositionPe = GetNodePosition(GetPlayerPeTF(enemyList));
                                                                                    if (LocalPosition == Vector3::Zero() || EnemyPosition == Vector3::Zero()) continue;
                                                                                    
                                                                                    float distanceToEnemy = Vector3::Distance(LocalPosition, EnemyPosition);
                                                                                    auto screenPos = WorldToScreenPoint(matrix, EnemyPosition);
                                                                                    float fovRadius = (pPlayer.AimFov / 180.0f) * (g_screenHeight / 2.0f);
                                                                                    
                                                                                    if (pPlayer.silen4a && isWithinFOV(screenPos, fovRadius) && distanceToEnemy <= 100.0f) {
                                                                                        if (distanceToEnemy < bestDistance) {
                                                                                            bestDistance = distanceToEnemy;
                                                                                            bestTargetPos = EnemyPosition;
                                                                                            bestTargetObj = enemyList;
                                                                                            hasValidTarget = true;
                                                                                        }
                                                                                    }
                                                                                    
                                                                                    auto LocationHeadBox = WorldToScreenPoint(matrix, EnemyPosition);
                                                                                    auto LocationToeBox = WorldToScreenPoint(matrix, EnemyPositionPe);
                                                                                    if (LocationHeadBox.X < -0 || LocationToeBox.X < -0) continue;
                                                                                    
                                                                                    auto *data = &SendResponse.Players[SendResponse.PlayerCount];
                                                                                    
                                                                                    memset(data->PlayerName, 0, 64);
                                                                                    if (isClientBot) {
                                                                                        strcpy(data->PlayerName, "Enemy");
                                                                                    } else {
                                                                                        uintptr_t NamePtr = Read<uintptr_t>(enemyList + 0x3d0); 
                                                                                        if (NamePtr > 0x10000000) {
                                                                                            getUTF8(data->PlayerName, NamePtr);
                                                                                        }
                                                                                        if (data->PlayerName[0] == '\0') strcpy(data->PlayerName, "Enemy");
                                                                                    }

                                                                                    data->Head = LocationHeadBox;
                                                                                    data->Toe = LocationToeBox;
                                                                                    data->IsCaido = isDieing;
                                                                                    data->Distancia = distanceToEnemy;
                                                                                    data->Name = isClientBot;
                                                                                    data->Health = Health;
                                                                                    
                                                                                    ++SendResponse.PlayerCount;
                                                                                    if (SendResponse.PlayerCount == maxplayerCount) break;
                                                                                }
                                                                            }
                                                                        }
                                                                    }
                                                                }
                                                            }
                                                        }
                                                    }
                                                    
                                                    if (pPlayer.silen4a && hasValidTarget) {
                                                        currentBestTargetPos = bestTargetPos;
                                                        currentBestTargetObj = bestTargetObj;
                                                        if (!keepAiming) {
                                                            keepAiming = true;
                                                            if (aimThread.joinable()) aimThread.join();  
                                                            aimThread = std::thread([&]() { ContinuousAim(localPlayer); });  
                                                        }
                                                    } else if (keepAiming) {
                                                        keepAiming = false;
                                                        if (aimThread.joinable()) aimThread.join();  
                                                        currentBestTargetPos = Vector3::Zero();  
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}

__attribute__ ((visibility ("hidden")))
int main(int argc, char*argv[]){
    if (InitServer() == (0)) {
        if (server.Accept()) {
            target_pid = FindPid("com.dts.freefiremax");
            if (target_pid > 0) {
                mem_fd = my_open_mem(target_pid);
                if (mem_fd <= 0) {
                    return 1;
                }
            } else {
                exit(1);
            }
            libAddress = FindLibrary("libil2cpp.so", 1);
            if (libAddress == 0) {
            }

            Request request{};
            while (server.receive((void*)&request) > (0)) {
                Response SendResponse{};
                if (request.Mode == Mode::InitMode) {
                    SendResponse.Success = true;
                } else if (request.Mode == Mode::HackMode) {
                    SendResponse.Success = true;
                } else if (request.Mode == Mode::EspMode) {
                    g_screenWidth = request.ScreenWidth;
                    g_screenHeight = request.ScreenHeight;
                    CreateDataList(SendResponse);
                    SendResponse.Success = true;
                } else if (request.Mode == 3) {
                    Actived.activar = request.m_IsOn;
                    SendResponse.Success = true;
                } else if(request.Mode == 5){
                    pPlayer.aimbot = request.m_IsOn;
                    SendResponse.Success = true;
                } else if(request.Mode == 22){
                    pPlayer.fovawm = request.m_IsOn;
                    SendResponse.Success = true;
                } else if(request.Mode == 54){
                    pPlayer.aimbotlock = request.m_IsOn;
                    SendResponse.Success = true;
                } else if(request.Mode == 57){
                    pPlayer.silen4a = request.m_IsOn;
                    SendResponse.Success = true;
                } else if(request.Mode == 58){
                    pPlayer.AimFov = (float)request.value;
                    SendResponse.Success = true;
                } else if(request.Mode == 59){
                    pPlayer.AimDistance = (float)request.value;
                    SendResponse.Success = true;
                } else if(request.Mode == 60){  // Speed hack toggle
                    speedHackEnabled = request.m_IsOn; // Get boolean value
                    SendResponse.Success = true;
                }
                server.send((void*)& SendResponse, sizeof(SendResponse));
            }
        }
    }
    return 0;
}
