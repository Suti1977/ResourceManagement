//------------------------------------------------------------------------------
// Copyright (c) 2026 [Istvan Suto]
//
// SPDX-License-Identifier: MIT
//------------------------------------------------------------------------------
//------------------------------------------------------------------------------
// ResourceLoop szoftveres idozitoi
// A timer manager a ResourceLoop taskjaban fut
//------------------------------------------------------------------------------
#ifndef RESOURCETIMER_H_
#define RESOURCETIMER_H_

#include "ResourceManager.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ResourceTimer ResourceTimer_t;
typedef struct ResourceTimerManager ResourceTimerManager_t;
typedef void ResourceTimer_expired_fn_t(void* callbackData);

//------------------------------------------------------------------------------
// Eroforrashoz kotott idozito
struct ResourceTimer
{
    // Igaz, ha a timer szerepel az aktiv idozitesek kozott.
    bool active;
    // Lejarati jelzes, amelyet a hivo elfogyaszthat.
    bool expired;
    // Igaz, ha a nulla intervallum mar beallitotta a lejart jelzest.
    // A manager feldolgozaskor nem allithatja be masodszor.
    bool immediateExpiryPending;
    // A kovetkezo lejarat abszolut 64 bites tickideje.
    uint64_t nextTime;
    // Ismetlodesi ido tickben; 0 = egyszeri timer.
    uint32_t periodTime;
    // Az utolso feldolgozo manager-kor generacioja.
    uint64_t processedGeneration;
    // A timerhez tartozo timer manager.
    ResourceTimerManager_t* manager;
    // A timerlista lockjat biztosito loop.
    ResourceLoop_t* loop;
    // Az eroforras, amelyhez a timer tartozik.
    Resource_t* owner;
    // Opcionalis callback a timer lejaratakor.
    ResourceTimer_expired_fn_t* expiredFunc;
    // A lejarati callback alkalmazasi kontextusa.
    void* callbackData;

    // A kovetkezo timer a lancolt listaban.
    ResourceTimer_t* next;
};
//------------------------------------------------------------------------------
// A timereket kezelo manager adatai.
struct ResourceTimerManager
{
    // Opcionalis lock-tulajdonos; NULL standalone hasznalatkor.
    ResourceLoop_t* loop;
    // A timer manager altal ismert aktualis abszolut ido.
    uint64_t time;
    // Legkozelebbi aktiv timer lejarta; UINT64_MAX = nincs.
    uint64_t nextExecutionTime;
    // Monoton generacio az egy manager-korben torteno feldolgozashoz.
    uint64_t processingGeneration;
    // A timerlista elso eleme.
    ResourceTimer_t* firstTimer;
    // A timerlista utolso eleme.
    ResourceTimer_t* lastTimer;
};
//------------------------------------------------------------------------------
// Ures timer manager inicializalasa.
void ResourceTimer_initManager(ResourceTimerManager_t* manager);

// Timer objektum egyszeri regisztralasa a resource loopjanak listajaban.
// Sikertelen, ha a resource nincs inicializalt loophoz rendelve.
// A timer egyszer hozhato letre, es a loop teljes elettartama alatt
// a listaban marad. Torolni vagy ujraregisztralni tilos.
// Az egyszeri regisztracio betartasa a hivo felelossege.
// A timer objektum nullazott, statikus memoriaju legyen.
bool ResourceTimer_createTimer(Resource_t* resource, ResourceTimer_t* timer);

// Timer inditasa/ujrainditasa:
//  interval: az elso lejarat; nulla eseten az expired azonnal igaz lesz,
//            a loop task a kovetkezo feldolgozasi korben kezeli a lejartat.
//  periodTime nulla eseten egyszeri, egyebkent periodikus timer.
// Az idok FreeRTOS tickben ertendok.
// Az idozites a manager aktualis 64 bites idejehez kepest szamitodik.

void ResourceTimer_start(ResourceTimer_t* timer,
                         uint32_t interval,
                         uint32_t periodTime);

// Timer leallitasa
void ResourceTimer_stop(ResourceTimer_t* timer);

// Lejarati jelzes lekerdezese. A rutin torli a lejarat jelzeset is.
// Az igaz visszateresi ertek csak egyszer olvashato ki, hacsak kozben ujabb
// lejarat nem tortenik.
bool ResourceTimer_expired(ResourceTimer_t* timer);

// Igaz erteket ad vissza, ha a timer aktiv.
bool ResourceTimer_isActive(ResourceTimer_t* timer);

// Idozites lejartakor meghivodo (opcionalis) callback regisztracioja.
// A callback a timer lejaratakor a loop taskjabol hivodik meg.
// A callbackData-nak a loop teljes elettartama alatt ervenyesnek kell lennie.
void ResourceTimer_registerExpiredFunc(ResourceTimer_t* timer,
                                       ResourceTimer_expired_fn_t* func,
                                       void* callbackData);

// A megadott 64 bites idopontig lejart timerek feldolgozasa. A timer callbackek
// a hivo loop taskjaban futnak.
void ResourceTimer_runManager(ResourceTimerManager_t* manager, uint64_t time);

// A legkozelebbi timer lejaratig tarto varakozasi ido lekerdezese tickben.
// (Megadja, hany tick mulva kell a loopnak legkozelebb felebrednie.)
// Nincs hatarido eseten portMAX_DELAY, mar lejart hataridonel nulla, nagy
// tavolsagnal pedig a FreeRTOS TickType_t altal reprezentalhato maximumot
// adja.
uint32_t ResourceTimer_getWaitTime(ResourceTimerManager_t* manager);

#ifdef __cplusplus
}
#endif
#endif
