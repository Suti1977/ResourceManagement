//------------------------------------------------------------------------------
// Copyright (c) 2026 [Istvan Suto]
//
// SPDX-License-Identifier: MIT
//------------------------------------------------------------------------------
//------------------------------------------------------------------------------
// ResourceLoop szoftveres idozitoi.
//------------------------------------------------------------------------------
#include "ResourceTimer.h"
#include "ResourceLoop.h"
#include <assert.h>
#include <string.h>

#define RESOURCE_TIMER_NO_DEADLINE UINT64_MAX

//------------------------------------------------------------------------------
// A modul valtozoit vedo mutex felvetele.
static void ResourceTimer_lock(ResourceTimerManager_t* manager)
{
    // A loophoz tartozo timer manager a loop mutexet hasznalja.
    // Onallo timer manager eseten nincs kulso mutex.
    if (manager->loop != NULL)
    {
        xSemaphoreTakeRecursive(manager->loop->mutex, portMAX_DELAY);
    }
}
//------------------------------------------------------------------------------
// A modul valtozoit vedo mutex feloldasa.
static void ResourceTimer_unlock(ResourceTimerManager_t* manager)
{
    if (manager->loop != NULL)
    {
        xSemaphoreGiveRecursive(manager->loop->mutex);
    }
}
//------------------------------------------------------------------------------
// A legkorabbi aktiv timer lejarati idejenek ujraszamolasa.
// A hivo feladata a manager lockolasa.
static void ResourceTimer_updateNextExecutionTime(
    ResourceTimerManager_t* manager)
{
    ResourceTimer_t* timer;

    manager->nextExecutionTime = RESOURCE_TIMER_NO_DEADLINE;
    for (timer = manager->firstTimer; timer != NULL; timer = timer->next)
    {
        if (timer->active &&
            (timer->nextTime < manager->nextExecutionTime))
        {
            manager->nextExecutionTime = timer->nextTime;
        }
    }
}
//------------------------------------------------------------------------------
// Ures timer manager inicializalasa.
void ResourceTimer_initManager(ResourceTimerManager_t* manager)
{
    assert(manager != NULL);
    // Az ures lista es a nulla ido alaphelyzetbe allitja a manager allapotat.
    memset(manager, 0, sizeof(*manager));

    // A UINT64_MAX azt jelenti, hogy jelenleg egyetlen aktiv timernek sincs
    // kovetkezo esedekessege.
    manager->nextExecutionTime = RESOURCE_TIMER_NO_DEADLINE;
}
//------------------------------------------------------------------------------
// Egy timer objektumot egyszer a resource loopjanak timer listajahoz kapcsol.
// Csak loophoz rendelt es inicializalt resource-hoz lehet timer objektumot
// letrehozni.
bool ResourceTimer_createTimer(Resource_t* resource, ResourceTimer_t* timer)
{
    ResourceLoopExtension_t* loopExtension;

    ResourceLoop_t* loop;

    assert(resource != NULL);
    assert(timer != NULL);
    loopExtension = resource->loopExtension;

    loop = (loopExtension != NULL) ?
           loopExtension->loop : NULL;
    if ((loop == NULL) || !loop->initialized)
    {
        return false;
    }

    // A timer lista modositasa ugyanazzal a mutexszel vedett, amely a loop
    // resource-listajat is vedheti. A nullazast is a lock alatt vegezzuk, hogy
    // a timer objektum ne valtozzon egy masik timer-muvelet kozben.
    xSemaphoreTakeRecursive(loop->mutex, portMAX_DELAY);

    memset(timer, 0, sizeof(*timer));
    timer->owner = resource;
    timer->manager = &loop->timerManager;
    timer->loop = loop;

    // Ha a timer egy lejarati callbackbol jon letre, ugyanabban a manager-
    // korben ne dolgozzuk fel ujra. A kovetkezo kor uj generaciot kap.
    timer->processedGeneration = loop->timerManager.processingGeneration;

    // A timer hozzaadasa a lancolt listahoz.
    timer->next = NULL;
    if (loop->timerManager.lastTimer != NULL)
    {
        loop->timerManager.lastTimer->next = timer;
    }
    else
    {
        loop->timerManager.firstTimer = timer;
    }
    loop->timerManager.lastTimer = timer;

    xSemaphoreGiveRecursive(loop->mutex);
    return true;
}
//------------------------------------------------------------------------------
// Timer inditasa/ujrainditasa:
//  interval: az elso lejarat,
//  periodTime nulla eseten egyszeri, egyebkent periodikus timer.
// Az idok FreeRTOS tickben ertendok.
// Az idozites a manager aktualis 64 bites idejehez kepest szamitodik.
void ResourceTimer_start(ResourceTimer_t* timer,
                         uint32_t interval,
                         uint32_t periodTime)
{
    ResourceLoop_t* loop;
    assert(timer != NULL);

    loop = timer->loop;
    if (loop != NULL)
    {
        xSemaphoreTakeRecursive(loop->mutex, portMAX_DELAY);
    }

    // Loophoz tartozo timernel a wrap-aroundt is kezelo 64 bites idoforrasbol
    // kerjuk le a hivas idejet. (A 32 bites tickbol utolag nem allithato vissza
    // tobb teljes korbefordulas.)
    {
        uint64_t now = timer->manager->time;
        if (loop != NULL)
        {
            now = loop->getTickCount64();
            timer->manager->time = now;
        }
        timer->expired = (interval == 0U);
        timer->immediateExpiryPending = (interval == 0U);
        timer->periodTime = periodTime;
        timer->nextTime = now + interval;
        timer->active = true;
    }

    // Ujrainditasnal a timer korabbi hatarideje is lehetett a legkorabbi.
    // Ezert minden inditas utan ujraszamoljuk a teljes aktiv timerlistat.
    ResourceTimer_updateNextExecutionTime(timer->manager);

    if (loop != NULL)
    {
        xSemaphoreGiveRecursive(loop->mutex);
        xTaskNotify(loop->taskHandle, 1U, eSetBits);
    }
}
//------------------------------------------------------------------------------
// Timer leallitasa
void ResourceTimer_stop(ResourceTimer_t* timer)
{
    // Leallitja az idozitot, de a timer objektumot a listaban hagyja.
    // A timer kesobb ujraindithato.

    ResourceLoop_t* loop;
    assert(timer != NULL);

    loop = timer->loop;
    if (loop != NULL)
    {
        xSemaphoreTakeRecursive(loop->mutex, portMAX_DELAY);
    }

    // A leallas torli a meg nem olvasott lejart jelzest is.
    timer->expired = false;
    timer->immediateExpiryPending = false;
    timer->active = false;

    // Ha ez volt a legkorabbi timer, a korabbi hatarido mar nem ervenyes.
    ResourceTimer_updateNextExecutionTime(timer->manager);

    if (loop != NULL)
    {
        xSemaphoreGiveRecursive(loop->mutex);
        xTaskNotify(loop->taskHandle, 1U, eSetBits);
    }
}

//------------------------------------------------------------------------------
// Lejarati jelzes lekerdezese. A rutin torli a lejarat jelzeset is.
// Az igaz visszateresi ertek csak egyszer olvashato ki, hacsak kozben ujabb
// lejarat nem tortenik.
bool ResourceTimer_expired(ResourceTimer_t* timer)
{
    bool expired;
    ResourceLoop_t* loop;
    assert(timer != NULL);

    loop = timer->loop;
    if (loop != NULL)
    {
        xSemaphoreTakeRecursive(loop->mutex, portMAX_DELAY);
    }

    expired = timer->expired;
    timer->expired = false;

    if (loop != NULL)
    {
        xSemaphoreGiveRecursive(loop->mutex);
    }

    return expired;
}
//------------------------------------------------------------------------------
// Az aktiv allapot lekerdezese a loop mutex vedelme alatt.
bool ResourceTimer_isActive(ResourceTimer_t* timer)
{
    bool active;
    ResourceLoop_t* loop;
    assert(timer != NULL);

    loop = timer->loop;
    if (loop != NULL)
    {
        xSemaphoreTakeRecursive(loop->mutex, portMAX_DELAY);
    }

    active = timer->active;

    if (loop != NULL)
    {
        xSemaphoreGiveRecursive(loop->mutex);
    }
    return active;
}
//------------------------------------------------------------------------------
// Idozites lejartakor meghivodo (opcionalis) callback regisztracioja.
// A callback a timer lejaratakor a loop taskjabol hivodik meg.
// A callbackData a timerhez tartozo tetszoleges adat.
void ResourceTimer_registerExpiredFunc(ResourceTimer_t* timer,
                                       ResourceTimer_expired_fn_t* func,
                                       void* callbackData)
{
    ResourceLoop_t* loop;
    assert(timer != NULL);

    loop = timer->loop;
    if (loop != NULL)
    {
        xSemaphoreTakeRecursive(loop->mutex, portMAX_DELAY);
    }

    timer->expiredFunc = func;
    timer->callbackData = callbackData;

    if (loop != NULL)
    {
        xSemaphoreGiveRecursive(loop->mutex);
    }
}
//------------------------------------------------------------------------------
// A megadott 64 bites idopontig lejart timerek feldolgozasa. A timer callbackek
// a hivo loop taskjaban futnak.
void ResourceTimer_runManager(ResourceTimerManager_t* manager, uint64_t time)
{
    ResourceTimer_t* timer;
    uint64_t generation;
    assert(manager != NULL);

    ResourceTimer_lock(manager);
    // Az idot a timer inditasakor masik task mar frissebbre allithatta.
    // A manager monoton ideje nem lephet visszafele egy korabban beolvasott
    // loop-idopont miatt.
    if (time > manager->time)
    {
        manager->time = time;
    }
    time = manager->time;

    manager->processingGeneration++;
    if (manager->processingGeneration == 0U)
    {
        // A nulla a frissen inicializalt timer feldolgozasi generacioja.
        manager->processingGeneration++;
    }
    generation = manager->processingGeneration;

    // Ha a legkorabbi ismert hatarido meg nem jott el, nincs esedekes timer.
    if (manager->nextExecutionTime > time)
    {
        ResourceTimer_unlock(manager);
        return;
    }
    ResourceTimer_unlock(manager);

    // Minden alkalommal a lista elejerol keressuk a kovetkezo esedekes timert.
    // A lista lock alatt vizsgalhato, de a felhasznaloi callbacket a mutexen
    // kivul hivjuk meg, igy nem alakulhat ki manager-lock / loop-lock holtpont.
    for(;;)
    {
        uint64_t scheduledTime;
        uint32_t scheduledPeriod;
        Resource_t* owner;
        ResourceTimer_expired_fn_t* expiredFunc;
        void* callbackData;

        ResourceTimer_lock(manager);

        // Lancolt lista vegigjarasa...
        timer = manager->firstTimer;
        while (timer != NULL)
        {
            if (timer->active &&
                (timer->nextTime <= time) &&
                (timer->processedGeneration != generation))
            {
                break;
            }
            timer = timer->next;
        }

        if (timer == NULL)
        {
            ResourceTimer_unlock(manager);
            break;
        }

        // A jelzest es a callback-adatokat meg a mutex alatt rogzitjuk.
        // A callback szabadon leallithat vagy ujraindithat timereket.
        timer->processedGeneration = generation;
        if (!timer->immediateExpiryPending)
        {
            timer->expired = true;
        }
        timer->immediateExpiryPending = false;
        scheduledTime = timer->nextTime;
        scheduledPeriod = timer->periodTime;
        owner = timer->owner;
        expiredFunc = timer->expiredFunc;
        callbackData = timer->callbackData;

        // A regi utemezest meg a callback elott allitjuk be. A timer objektum
        // a loop elettartama alatt ervenyes, ezert a callback biztonsagosan
        // leallithatja vagy ujraindithatja.
        if (scheduledPeriod != 0U)
        {
            // A periodus a korabbi hataridohoz kepest lep tovabb. Keseskor
            // minden manager-kor legfeljebb egy lejaratot dolgoz fel.
            timer->nextTime = scheduledTime + scheduledPeriod;
        }
        else
        {
            // Az egyszeri timer a callback kezdetere inaktivva valik. A
            // callback szukseg eseten explicit modon ujraindithatja.
            timer->active = false;
        }

        ResourceTimer_unlock(manager);

        if (owner != NULL)
        {
            ResourceLoop_setTimerExpired(owner);
        }
        if (expiredFunc != NULL)
        {
            expiredFunc(callbackData);
        }
    }

    // A callbackek leallitottak vagy ujraindithattak timereket.
    // A listabol ujraszamoljuk a legkorabbi aktiv hataridot.
    ResourceTimer_lock(manager);
    ResourceTimer_updateNextExecutionTime(manager);
    ResourceTimer_unlock(manager);
}
//------------------------------------------------------------------------------
// A legkozelebbi timer lejaratig tarto varakozasi ido lekerdezese tickben.
// (Megadja, hany tick mulva kell a loopnak legkozelebb felebrednie.)
// Nincs hatarido eseten portMAX_DELAY, mar lejart hataridonel nulla, nagy
// tavolsagnal pedig a FreeRTOS TickType_t altal reprezentalhato maximumot
// adja.
uint32_t ResourceTimer_getWaitTime(ResourceTimerManager_t* manager)
{
    uint64_t waitTime;
    uint32_t result;
    assert(manager != NULL);

    ResourceTimer_lock(manager);

    if (manager->nextExecutionTime == RESOURCE_TIMER_NO_DEADLINE)
    {
        result = (uint32_t)portMAX_DELAY;
    }
    else if (manager->nextExecutionTime <= manager->time)
    {
        result = 0U;
    }
    else
    {
        waitTime = manager->nextExecutionTime - manager->time;
        if (waitTime >= (uint64_t)portMAX_DELAY)
        {
            // A portMAX_DELAY egyes FreeRTOS konfiguraciokban vegtelen
            // varakozas.
            // Hosszu, de veges hataridot reszletekben varunk ki.
            result = (uint32_t)(portMAX_DELAY - 1U);
        }
        else
        {
            result = (uint32_t)waitTime;
        }
    }

    ResourceTimer_unlock(manager);

    return result;
}
//------------------------------------------------------------------------------
