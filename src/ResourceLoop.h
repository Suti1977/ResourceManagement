//------------------------------------------------------------------------------
// Copyright (c) 2026 [Istvan Suto]
//
// SPDX-License-Identifier: MIT
//------------------------------------------------------------------------------
//------------------------------------------------------------------------------
// Kooperativ resource-ok kozos FreeRTOS taskja
//
// Egy ResourceLoop taskban, sorosan futtatja a hozzarendelt Resource_t
// allapotgepeket. A callbackek nem futhatnak parhuzamosan ugyanazon loopon
// belul, ezert minden callbacknek rovid ideig szabad csak futnia.
//------------------------------------------------------------------------------
#ifndef RESOURCELOOP_H_
#define RESOURCELOOP_H_

#include "ResourceManager.h"
#include "task.h"
#include "ResourceTimer.h"

#if !configSUPPORT_STATIC_ALLOCATION
#error "ResourceLoop requires configSUPPORT_STATIC_ALLOCATION=1"
#endif

#ifdef __cplusplus
extern "C" {
#endif

//------------------------------------------------------------------------------
// Loop inicializalo struktura
typedef struct
{
    // Diagnosztikai nev; a stringnek tartosnak kell lennie.
    const char* name;
    // A FreeRTOS task prioritas-szintje.
    uint32_t taskPriority;
    // A stack merete StackType_t szavakban.
    uint32_t taskStackSize;
    // A hivo altal statikusan biztositott task stack.
    // A stack buffernek a loop teljes elettartama alatt ervenyesnek kell
    // lennie!
    StackType_t* taskStackBuffer;
    // Monoton 64 bites tickforras, wrap-aroundtol fuggetlenul.
    uint64_t (*getTickCount64)(void);
} ResourceLoopConfig_t;
//------------------------------------------------------------------------------
struct ResourceLoop
{
    // A FreeRTOS task statikus TCB-taroloja.
    StaticTask_t taskBuffer;
    // A loop rekurziv mutexenek statikus taroloja.
    StaticSemaphore_t mutexBuffer;
    // A letrehozott FreeRTOS task handle-je.
    TaskHandle_t taskHandle;
    // A loop allapotat vedo rekurziv mutex handle-je.
    SemaphoreHandle_t mutex;
    // Diagnosztikai nev; a stringnek tartosnak kell lennie.
    const char* name;
    // A loophoz rendelt resource-ok elso eleme.
    Resource_t* resources;
    // A loophoz rendelt resource-ok utolso eleme.
    Resource_t* lastResource;
    // A loophoz tartozo szoftveres timerek kezeloi.
    ResourceTimerManager_t timerManager;
    // Monoton, 64 bites FreeRTOS tickforrast ado callback
    uint64_t (*getTickCount64)(void);
    // Az idoforras legutobb beolvasott abszolut tickje.
    uint64_t time64;
    // Igaz, ha a loop sikeresen inicializalva lett.
    bool initialized;
};
//------------------------------------------------------------------------------
// Inicializalja a loopot es letrehozza a taskot. Az osszes resource hozzaadasa
// a ResourceManager futtatasanak megkezdese elott tortenjen.
bool ResourceLoop_init(ResourceLoop_t* loop,
                       const ResourceLoopConfig_t* config);

// Egy mar managerhez regisztralt resource-t a loop listajahoz fuz.
// A ResourceConfig_t.loopExtension mutasson statikus, egyedi objektumra.
// Egy resource legfeljebb egy loophoz tartozhat.
bool ResourceLoop_addResource(ResourceLoop_t* loop, Resource_t* resource);

// Esemeny kuldese egy loop alatt futo eroforrasnak.
// Bitenkenti OR muvelettel alkalmazasi esemenyt ad a loophoz rendelt
// resource-hoz.
// Csak task kontextusbol hivhato.
void ResourceLoop_setEvent(Resource_t* resource, uint64_t events);

// ISR-biztos valtozat; csak ISR-bol hivhato.
void ResourceLoop_setEventFromIsr(Resource_t* resource, uint64_t events);

//------------------------------------------------------------------------------
//                  ---- BELSO HASZNALATU FUGGVENYEK! ----
// A manager start kerelmet tovabbitja a loopnak a megadott eroforrasra.
void ResourceLoop_setStartRequest(Resource_t* resource);
// A manager stop kerelmet tovabbitja a loopnak a megadott eroforrasra.
void ResourceLoop_setStopRequest(Resource_t* resource);
// Timer-lejaratkor a tulajdonos resource feldolgozasra jelolese.
void ResourceLoop_setTimerExpired(Resource_t* resource);
//------------------------------------------------------------------------------
#ifdef __cplusplus
}
#endif
#endif
