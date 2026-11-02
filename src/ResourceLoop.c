//------------------------------------------------------------------------------
// Copyright (c) 2026 [Istvan Suto]
//
// SPDX-License-Identifier: MIT
//------------------------------------------------------------------------------
//------------------------------------------------------------------------------
// Kooperativ resource-ok kozos FreeRTOS taskja.
//
// A modul egy taskban futtatja a hozzarendelt Resource_t objektumokat.
// Az alkalmazasi callbackek ebbol a taskbol, sorosan futnak. A manager csak
// lifecycle-kerest tovabbit; a callbackeket nem hivja kozvetlenul.
// A taskertesites ebresztesre szolgal; az esemenyadatok a Resource_t
// objektumban tarolt bitmaszkokban vannak, igy az azonos esemenyek
// osszevonodnak.
//------------------------------------------------------------------------------
#include "ResourceLoop.h"
#include <assert.h>
#include <string.h>
#include <limits.h>
#include "task.h"

// Modult vezerlo esemeny bitek
#define RESOURCE_LOOP_NOTIFY_WORK       (1UL << 0)
// Az eroforras esemenyt kapott.
#define RESOURCE_LOOP_EVENT_INPUT       (1UL << 0)
// Az eroforrashoz tartozo valamelyik timer lejart.
#define RESOURCE_LOOP_EVENT_TIMER       (1UL << 1)
// Az eroforras a managertol inditasi kerelmet kapott.
#define RESOURCE_LOOP_EVENT_START       (1UL << 2)
// Az eroforras a managertol leallitasi kerelmet kapott.
#define RESOURCE_LOOP_EVENT_STOP        (1UL << 3)

static void ResourceLoop_task(void* taskParam);
static void ResourceLoop_runResource(ResourceLoop_t* loop,
                                     Resource_t* resource);
static void ResourceLoop_changeState(Resource_t* resource,
                                     ResourceLoopState_t state);
static void ResourceLoop_reportState(Resource_t* resource,
                                     ResourceState_t state,
                                     status_t status);
static void ResourceLoop_scheduleTimer(ResourceLoop_t* loop,
                                       Resource_t* resource);
static bool ResourceLoop_takeStopRequest(ResourceLoopExtension_t* loopExtension,
                                         uint32_t controlEvents);
static void ResourceLoop_completeDone(ResourceLoop_t* loop,
                                      Resource_t* resource,
                                      uint32_t controlEvents);

//------------------------------------------------------------------------------
// Inicializalja a loop allapotat es elinditja a kozos taskot.
//
// Eloszor minden task altal hasznalt adatot es szinkronizacios objektumot
// keszit elo, es csak ezutan hozza letre a taskot. Ez elkeruli, hogy a task
// meg a mutex vagy a timer manager inicializalasa elott fusson eloszor.
// Igaz ertekkel ter vissza, ha a task es minden szukseges objektum
// sikeresen letrejott.
bool ResourceLoop_init(ResourceLoop_t* loop,
                       const ResourceLoopConfig_t* config)
{
    configSTACK_DEPTH_TYPE stackDepth;
    UBaseType_t taskPriority;

    if ((loop == NULL) || (config == NULL) || (config->name == NULL) ||
        (config->taskStackSize == 0U) ||
        (config->taskStackBuffer == NULL) ||
        (config->getTickCount64 == NULL))
    {
        return false;
    }

    stackDepth = (configSTACK_DEPTH_TYPE)config->taskStackSize;
    taskPriority = (UBaseType_t)config->taskPriority;
    if (((uint32_t)stackDepth != config->taskStackSize) ||
        ((uint32_t)taskPriority != config->taskPriority))
    {
        return false;
    }

    // A nullazas az osszes lista, timer es allapot kezdeti erteket beallitja.
    memset(loop, 0, sizeof(*loop));

    loop->name = config->name;

    // Az idoziteseket kezelo manager inicializalasa.
    ResourceTimer_initManager(&loop->timerManager);
    // A 64 bites idot a platform wrap-aroundt is kezelo forrasa adja.
    // (Egy 32 bites tick ket mintajabol nem lehet tobb teljes korbefordulast
    // visszaallitani, ezert a modul nem probalja sajat maga kitalalni az idot.)
    loop->getTickCount64 = config->getTickCount64;
    // Az aktualis ido beallitasa a manager szamara mar inicializalaskor.
    loop->time64 = loop->getTickCount64();    
    loop->timerManager.time = loop->time64;

    // A loop listajat es a timer listat kozos mutex vedheti. Recursive mutex
    // kell, mert egyes timer muveletek a loop lock birtokaban is meghivhatok.
    loop->mutex = xSemaphoreCreateRecursiveMutexStatic(&loop->mutexBuffer);
    if (loop->mutex == NULL)
    {
        return false;
    }

    // A timer manager igy tudja a sajat listajat vedo mutexet elerni.
    loop->timerManager.loop = loop;

    // Loop-ot futtato taszk letrehozasa
    loop->taskHandle = xTaskCreateStatic(ResourceLoop_task,
                                         config->name,
                                         stackDepth,
                                         loop,
                                         taskPriority,
                                         config->taskStackBuffer,
                                         &loop->taskBuffer);
    if (loop->taskHandle == NULL)
    {
        vSemaphoreDelete(loop->mutex);
        loop->mutex = NULL;
        return false;
    }

    // A task csak a sikeres letrehozas utan tekintheto hasznalhatonak.
    loop->initialized = true;
    return true;
}
//------------------------------------------------------------------------------
// Egy mar managerhez regisztralt resource-t a loop listajahoz fuz.
// Egy resource legfeljebb egy loophoz tartozhat.
bool ResourceLoop_addResource(ResourceLoop_t* loop, Resource_t* resource)
{
    ResourceLoopExtension_t* loopExtension;

    assert(loop != NULL);
    assert(resource != NULL);
    loopExtension = resource->loopExtension;

    if (!loop->initialized || resource->manager == NULL ||
        resource->manager->running || (loopExtension == NULL) ||
        (loopExtension->loop != NULL))
    {
        return false;
    }

    xSemaphoreTakeRecursive(loop->mutex, portMAX_DELAY);

    if (loopExtension->loop != NULL)
    {
        xSemaphoreGiveRecursive(loop->mutex);
        return false;
    }

    loopExtension->loop = loop;
    loopExtension->loopNext = NULL;
    loopExtension->loopGeneration = resource->lifecycleGeneration;

    // A resource loop pointere jelzi a ResourceManager fele is, hogy az
    // esemenyeket es start/stop kerelmeket a loop tasknak kell tovabbitani.
    // Kulon loopNext lancot hasznalunk; a manager sajat listait nem erintjuk.

    if (loop->lastResource != NULL)
    {
        ResourceLoopExtension_t* lastLoopExtension =
            loop->lastResource->loopExtension;
        lastLoopExtension->loopNext = resource;
    }
    else
    {
        loop->resources = resource;
    }
    loop->lastResource = resource;

    xSemaphoreGiveRecursive(loop->mutex);

    // Az uj tag alapallapotanak feldolgozasahoz azonnal felebresztjuk a taskot.
    xTaskNotify(loop->taskHandle, RESOURCE_LOOP_NOTIFY_WORK, eSetBits);
    return true;
}
//------------------------------------------------------------------------------
// Esemeny kuldese egy Loop alatt futo eroforrasnak
// Bitenkenti OR muvelettel alkalmazasi esemenyt ad a loopos resource-hoz.
// Csak task kontextusbol hivhato.
void ResourceLoop_setEvent(Resource_t* resource, uint64_t events)
{
    ResourceLoopExtension_t* loopExtension;

    ResourceLoop_t* loop;
    assert(resource != NULL);
    loopExtension = resource->loopExtension;

    loop = (loopExtension != NULL) ?
           loopExtension->loop : NULL;
    if ((loop == NULL) || (events == 0U))
    {
        return;
    }

    // Az alkalmazasi esemenyek kulon uint64_t maszkban gyulnek. A vezerlobit
    // csak azt jelzi, hogy uj input erkezett; maga az adat a maszkban van.
    taskENTER_CRITICAL();
    resource->inputEventsAsync |= events;
    loopExtension->loopControlEventsAsync |=
        RESOURCE_LOOP_EVENT_INPUT;
    taskEXIT_CRITICAL();

    // Loop ebresztese
    xTaskNotify(loop->taskHandle, RESOURCE_LOOP_NOTIFY_WORK, eSetBits);
}
//------------------------------------------------------------------------------
// ISR-biztos esemenykuldes.
// A resource adatainak modositasa a FreeRTOS ISR critical-section API-javal
// vedett. Az ISR-bol csak az erre alkalmas task-notification API hasznalhato;
// a magasabb prioritasu task felkelteset a portYIELD_FROM_ISR jelzi.
void ResourceLoop_setEventFromIsr(Resource_t* resource, uint64_t events)
{
    ResourceLoopExtension_t* loopExtension;

    ResourceLoop_t* loop;
    BaseType_t higherPriorityTaskWoken = pdFALSE;
    UBaseType_t interruptStatus;

    assert(resource != NULL);
    loopExtension = resource->loopExtension;
    loop = (loopExtension != NULL) ?
           loopExtension->loop : NULL;
    if ((loop == NULL) || (events == 0U))
    {
        return;
    }

    interruptStatus = taskENTER_CRITICAL_FROM_ISR();
    resource->inputEventsAsync |= events;
    loopExtension->loopControlEventsAsync |=
        RESOURCE_LOOP_EVENT_INPUT;
    taskEXIT_CRITICAL_FROM_ISR(interruptStatus);

    (void)xTaskNotifyFromISR(loop->taskHandle,
                             RESOURCE_LOOP_NOTIFY_WORK,
                             eSetBits,
                             &higherPriorityTaskWoken);
    portYIELD_FROM_ISR(higherPriorityTaskWoken);
}
//------------------------------------------------------------------------------
// A manager start kerelmet tovabbitja a loopnak a megadott eroforrasra.
void ResourceLoop_setStartRequest(Resource_t* resource)
{
    ResourceLoopExtension_t* loopExtension;

    ResourceLoop_t* loop;
    assert(resource != NULL);
    loopExtension = resource->loopExtension;
    loop = (loopExtension != NULL) ?
           loopExtension->loop : NULL;
    if (loop == NULL)
    {
        return;
    }

    // A generaciot itt rogzitjuk, hogy a loop csak az aktualis eletciklus
    // allapotat jelentse vissza, es egy regi futas kesoi eredmenye ne irja
    // felulirja az ujat.
    taskENTER_CRITICAL();
    loopExtension->loopGeneration = resource->lifecycleGeneration;
    loopExtension->loopControlEventsAsync |=
        RESOURCE_LOOP_EVENT_START;
    taskEXIT_CRITICAL();

    // Loop taszk ebresztese
    xTaskNotify(loop->taskHandle, RESOURCE_LOOP_NOTIFY_WORK, eSetBits);
}
//------------------------------------------------------------------------------
// A manager stop kerelmet tovabbitja a loopnak a megadott eroforrasra.
void ResourceLoop_setStopRequest(Resource_t* resource)
{
    ResourceLoopExtension_t* loopExtension;

    // A tenyleges stopFunc nem itt fut: azt majd a loop task hajtja vegre,
    // amikor a resource allapotgepe szerint a leallitas esedekes.
    ResourceLoop_t* loop;
    assert(resource != NULL);
    loopExtension = resource->loopExtension;
    loop = (loopExtension != NULL) ?
           loopExtension->loop : NULL;
    if (loop == NULL)
    {
        return;
    }

    taskENTER_CRITICAL();
    loopExtension->loopControlEventsAsync |= RESOURCE_LOOP_EVENT_STOP;
    taskEXIT_CRITICAL();

    // Loop taszk ebresztese
    xTaskNotify(loop->taskHandle, RESOURCE_LOOP_NOTIFY_WORK, eSetBits);
}
//------------------------------------------------------------------------------
// Timer-lejaratkor a tulajdonos resource feldolgozasra jelolese.
void ResourceLoop_setTimerExpired(Resource_t* resource)
{
    ResourceLoopExtension_t* loopExtension;

    // A timer manager nem hajtja vegre kozvetlenul a resource allapotgepet,
    // csak vezerloesemenyt tesz fel es ebreszti a loop taskot.
    ResourceLoop_t* loop;
    assert(resource != NULL);
    loopExtension = resource->loopExtension;
    loop = (loopExtension != NULL) ?
           loopExtension->loop : NULL;
    if (loop == NULL)
    {
        return;
    }

    taskENTER_CRITICAL();
    loopExtension->loopControlEventsAsync |=
        RESOURCE_LOOP_EVENT_TIMER;
    taskEXIT_CRITICAL();

    // Loop taszk ebresztese
    xTaskNotify(loop->taskHandle, RESOURCE_LOOP_NOTIFY_WORK, eSetBits);
}
//------------------------------------------------------------------------------
// A loop fo taskja. Minden ebredeskor frissiti a monoton 64 bites idot,
// feldolgozza a lejart timereket es vegigjarja a resource listat. A callbackek
// sorosan futnak, ezert rovidek es nem blokkolok kell legyenek. A user
// callbackek alatt a loop mutex nincs zarvan,  igy elkerulheto a manager es a
// loop mutex kozotti lock-inverzio.
static void ResourceLoop_task(void* taskParam)
{
    ResourceLoop_t* loop = (ResourceLoop_t*)taskParam;
    uint32_t waitTime = portMAX_DELAY;
    uint32_t notifications;

    for (;;)
    {
        Resource_t* resource;
        uint64_t nextResourceDeadline = UINT64_MAX;
        uint64_t now;
        uint64_t remaining;
        uint32_t timerWait;
        uint32_t resourceWait;

        // A taskertesites csak ebresztesre szolgal; az esemenyadatok a
        // Resource_t-ben tarolt maszkokbol kerulnek atvetelre.
        (void)xTaskNotifyWait(0U, UINT32_MAX, &notifications, waitTime);

        // Az uj varakozasi idot minden korben ujraszamitjuk. A korabbi
        // ertek csak az elozo xTaskNotifyWait hivas varakozasat szabalyozta.
        waitTime = portMAX_DELAY;

        // A platform altal szolgaltatott 64 bites monoton tickido lekerdezese.
        // Azert kell a 64 bit, mert az akkor is helyes marad, ha a task egy
        // vagy tobb teljes 32 bites tickperiodust aludt at vagy nem kapott
        // CPU-idot.
        now = loop->getTickCount64();
        xSemaphoreTakeRecursive(loop->mutex, portMAX_DELAY);
        if (now < loop->timerManager.time)
        {
            now = loop->timerManager.time;
        }
        else
        {
            loop->timerManager.time = now;
        }
        loop->time64 = now;
        xSemaphoreGiveRecursive(loop->mutex);

        // Az idoziteseket kezelo manager futtatasa.
        ResourceTimer_runManager(&loop->timerManager, loop->time64);

        // A resource-lista csak a vegehez bovulhet. A kovetkezo pointert a
        // mutex alatt mentjuk el, a resource allapotgepet viszont mutex nelkul
        // futtatjuk, mert allapotot jelenthet a managernek es user callbacket
        // valthat ki. Ha kozben uj tag kerul a listara, a hozza tartozo notify
        // miatt a kovetkezo ciklus azt is feldolgozza.
        xSemaphoreTakeRecursive(loop->mutex, portMAX_DELAY);
        resource = loop->resources;
        xSemaphoreGiveRecursive(loop->mutex);

        // Vegig at eroforrasok kistajan...
        while (resource != NULL)
        {
            ResourceLoopExtension_t* loopExtension =
                resource->loopExtension;
            Resource_t* next;

            xSemaphoreTakeRecursive(loop->mutex, portMAX_DELAY);
            next = loopExtension->loopNext;
            xSemaphoreGiveRecursive(loop->mutex);

            ResourceLoop_runResource(loop, resource);

            // A callbackban beallitott sajat hataridoket abszolut idokent
            // gyujtjuk. A varakozasi idot csak a teljes lista feldolgozasa
            // utan, friss monoton idohoz viszonyitva szamitjuk ki.
            if (loopExtension->loopTimerActive &&
                (loopExtension->loopTimerDue < nextResourceDeadline))
            {
                nextResourceDeadline = loopExtension->loopTimerDue;
            }
            resource = next;
        }

        // A callbackek kozben a ResourceTimer_start() uj hataridot allithat
        // be. Ezert a timer manager varakozasi idejet nem a callbackek elott,
        // hanem utanuk kell lekerdezni. A manager idejet is frissitjuk, hogy
        // a callbackek alatt lejart timerek nulla varakozast eredmenyezzenek.
        now = loop->getTickCount64();
        xSemaphoreTakeRecursive(loop->mutex, portMAX_DELAY);
        if (now < loop->timerManager.time)
        {
            now = loop->timerManager.time;
        }
        else
        {
            loop->timerManager.time = now;
        }
        loop->time64 = now;
        xSemaphoreGiveRecursive(loop->mutex);

        timerWait = ResourceTimer_getWaitTime(&loop->timerManager);
        if (timerWait < waitTime)
        {
            waitTime = timerWait;
        }

        if (nextResourceDeadline != UINT64_MAX)
        {
            remaining = (nextResourceDeadline <= now) ?
                        0U : nextResourceDeadline - now;
            resourceWait =
                (remaining >= (uint64_t)portMAX_DELAY) ?
                (uint32_t)(portMAX_DELAY - 1U) : (uint32_t)remaining;
            if (resourceWait < waitTime)
            {
                waitTime = resourceWait;
            }
        }
    }
}
//------------------------------------------------------------------------------
// Allapotvaltas es a kovetkezo allapot egyszeri inicializalasanak kerese.
// A loopStateInitPending megkulonbozteti az allapotba belepest az allapotban
// torteno tovabbi futasoktol.
static void ResourceLoop_changeState(Resource_t* resource,
                                     ResourceLoopState_t state)
{
    ResourceLoopExtension_t* loopExtension = resource->loopExtension;

    loopExtension->loopState = state;
    loopExtension->loopStateInitPending = true;
}
//------------------------------------------------------------------------------
// A loop eredmenyet az aktualis lifecycle-generaciohoz kotve jelenti a
// managernek. A generacios ellenorzes a regi callbackek kesoi visszajelzeset
// szuri ki egy stop/restart utan.
static void ResourceLoop_reportState(Resource_t* resource,
                                     ResourceState_t state,
                                     status_t status)
{
    uint32_t generation;
    taskENTER_CRITICAL();
    generation = resource->loopExtension->loopGeneration;
    taskEXIT_CRITICAL();

    Resource_setStateForGeneration(resource, generation, state, status);
}
//------------------------------------------------------------------------------
// Beallitja a resource kovetkezo loop-hataridejet.
// portMAX_DELAY eseten nincs sajat hatarido, ezert csak esemeny vagy belso
// vezerlojel ebresztheti. Egyebkent a waitTime-t a 64 bites loop idohoz
// adjuk.
static void ResourceLoop_scheduleTimer(ResourceLoop_t* loop,
                                       Resource_t* resource)
{
    ResourceLoopExtension_t* loopExtension = resource->loopExtension;
    ResourceControl_t* loopControl = &loopExtension->loopControl;

    if (loopControl->waitTime == portMAX_DELAY)
    {
        loopExtension->loopTimerActive = false;
    }
    else
    {
        uint64_t now = loop->getTickCount64();

        // A varakozasi ido a callback befejezese utan indul. A task elejen
        // beolvasott ido a callback futasa alatt elavulhat, ezert friss
        // idopontot veszunk, es a timer managerrel kozos lock alatt
        // szinkronizaljuk a monoton idoerteket.
        xSemaphoreTakeRecursive(loop->mutex, portMAX_DELAY);
        if (now < loop->timerManager.time)
        {
            now = loop->timerManager.time;
        }
        else
        {
            loop->timerManager.time = now;
        }
        loop->time64 = now;
        xSemaphoreGiveRecursive(loop->mutex);

        loopExtension->loopTimerActive = true;
        loopExtension->loopTimerDue = now +
            (uint64_t)loopControl->waitTime;
    }
}
//------------------------------------------------------------------------------
// Atveszi a STOP kerest akkor is, ha az az aktualis callback futasa kozben
// erkezett. Az atvetel soran a duplikalt STOP biteket is osszevonja.
static bool ResourceLoop_takeStopRequest(ResourceLoopExtension_t* loopExtension,
                                         uint32_t controlEvents)
{
    bool stopRequested =
        ((controlEvents & RESOURCE_LOOP_EVENT_STOP) != 0U);

    taskENTER_CRITICAL();
    if ((loopExtension->loopControlEventsAsync &
         RESOURCE_LOOP_EVENT_STOP) != 0U)
    {
        stopRequested = true;
        loopExtension->loopControlEventsAsync &=
            ~RESOURCE_LOOP_EVENT_STOP;
    }
    taskEXIT_CRITICAL();

    return stopRequested;
}
//------------------------------------------------------------------------------
// Egy befejezett resource terminalis DONE allapotba leptetese. A callback
// futasa kozben beerkezett STOP kerest a DONE elott kell ervenyesiteni.
static void ResourceLoop_completeDone(ResourceLoop_t* loop,
                                      Resource_t* resource,
                                      uint32_t controlEvents)
{
    ResourceLoopExtension_t* loopExtension = resource->loopExtension;

    if (ResourceLoop_takeStopRequest(loopExtension, controlEvents))
    {
        ResourceLoop_changeState(resource, RESOURCE_LOOP_STOPPING);
        xTaskNotify(loop->taskHandle, RESOURCE_LOOP_NOTIFY_WORK, eSetBits);
        return;
    }

    ResourceLoop_reportState(resource, RESOURCE_DONE_, kStatus_Success);
    ResourceLoop_changeState(resource, RESOURCE_LOOP_WAITING_FOR_START);
    loopExtension->loopTimerActive = false;
    xTaskNotify(loop->taskHandle, RESOURCE_LOOP_NOTIFY_WORK, eSetBits);
}
//------------------------------------------------------------------------------
// Egyetlen resource allapotgepenek egy koros futtatasa.
// Eloszor atomikusan atveszi es torli a fuggo esemenymaszkokat, majd eldonti,
// van-e ok callbackre. Az INPUT csak ebresztojelzes; a loopFunc futasat az
// alkalmazasi esemeny es a waitedEvents maszk metszete valtja ki. A nem vart
// esemeny a task ebredese utan elveszik. A start/stop/timer jelek fuggetlenek.
static void ResourceLoop_runResource(ResourceLoop_t* loop,
                                     Resource_t* resource)
{
    ResourceLoopExtension_t* loopExtension = resource->loopExtension;
    ResourceControl_t* loopControl = &loopExtension->loopControl;

    uint32_t controlEvents;
    uint64_t events;
    uint64_t matchedEvents;
    bool timed = false;
    bool shouldRun;
    status_t status = kStatus_Success;

    // Az esemenyek atadasa egyetlen kritikus szakaszban tortenik. A maszkok
    // torlese utan ujonnan erkezo esemenyek mar a kovetkezo korhoz tartoznak.
    taskENTER_CRITICAL();
    controlEvents = loopExtension->loopControlEventsAsync;
    loopExtension->loopControlEventsAsync = 0U;
    events = resource->inputEventsAsync;
    resource->inputEventsAsync = 0U;
    taskEXIT_CRITICAL();

    // Eltarolja az esemenyeket az eroforras szamara.
    loopExtension->loopControlEvents = controlEvents;

    // A resource sajat waitTime idoziteset kulon ellenorizzuk a kozos timer
    // manager esedekessegeitol fuggetlenul. Lejaratkor a timed jelzest
    // allitjuk.
    if (loopExtension->loopTimerActive &&
        (loopExtension->loopTimerDue <= loop->time64))
    {
        loopExtension->loopTimerActive = false;
        controlEvents |= RESOURCE_LOOP_EVENT_TIMER;
    }

    // A ResourceTimer altal jelzett timer-lejarat is idozitett futasnak
    // szamit, nem csak a resource sajat waitTime idozitojenek lejarata.
    timed = (controlEvents & RESOURCE_LOOP_EVENT_TIMER) != 0U;

    matchedEvents = events & loopControl->waitedEvents;

    shouldRun =
        (matchedEvents != 0U) ||
                timed ||
                ((controlEvents & (RESOURCE_LOOP_EVENT_START |
                                   RESOURCE_LOOP_EVENT_STOP |
                                   RESOURCE_LOOP_EVENT_TIMER)) != 0U) ||
                loopExtension->loopStateInitPending ||
                ((loopExtension->loopState == RESOURCE_LOOP_RUNNING) &&
                 loopControl->done) ||
                ((loopExtension->loopState == RESOURCE_LOOP_STOPPING) &&
                 !loopControl->prohibitStop);

    // Az INPUT onmagaban nem futtat callbacket. Ha az esemeny nem
    // illeszkedik a waitedEvents maszkra, az atvett esemeny elveszik.
    if (!shouldRun)
    {
        return;
    }

    // Csak a vart esemenymaszkra illeszkedo bitek juthatnak el a callbackig.
    // Mas callback-indito ok (pl. timer vagy STOP) nem szivarogtathat at
    // nem vart alkalmazasi esemenyeket.
    loopControl->events = matchedEvents;
    loopControl->timed = timed;
    loopControl->waitedEvents = 0U;
    loopControl->waitTime = portMAX_DELAY;

    if (loopExtension->loopStateInitPending)
    {
        // Az allapotgepet inicializalni kell.

        loopExtension->loopStateInitPending = false;

        switch (loopExtension->loopState)
        {
            case RESOURCE_LOOP_WAITING_FOR_START:
                // Varakozo allapotban nincs futasidozito: csak startkeres vagy
                // az allapot kezdeti inicializalasa indithatja el a kovetkezo
                // lepest.
                loopControl->waitedEvents = 0U;
                loopControl->waitTime = portMAX_DELAY;
                loopExtension->loopErrorCode = kStatus_Success;
                loopExtension->loopTimerActive = false;
                break;

            case RESOURCE_LOOP_STARTING:
                // Uj startkiserletkor minden korabbi futasi jelzest
                // alaphelyzetbe allitunk, mielott a felhasznaloi startFunc
                // megkapja a controlt.
                loopControl->waitTime = 0U;
                loopControl->waitedEvents = 0U;
                loopControl->done = false;
                loopControl->prohibitStop = false;
                loopControl->stopRequest = false;
                loopControl->timed = false;
                loopControl->errorClosed = false;
                loopControl->run = true;
                loopExtension->loopErrorCleared = false;
                loopExtension->loopErrorCode = kStatus_Success;

                // A startFunc az inditas tenyleges megkezdeseert felel. A run
                // flaggel jelezheti, hogy mar futhat-e a RUNNING allapot, vagy
                // tovabbi loopFunc korokre van szukseg az aszinkron inditashoz.
                if (loopExtension->startFunc != NULL)
                {
                    // Start callback futtatasa
                    status = loopExtension->startFunc(
                        resource->callbackData,
                        loopControl);
                    if (status != kStatus_Success)
                    {
                        goto error;
                    }
                }

                if (loopControl->run)
                {
                    // Az eroforras a startFunc alatt nem torolte a run flaget,
                    // igy az eroforras jelezheti, hogy elindult.
                    ResourceLoop_reportState(resource,
                                             RESOURCE_RUNNING,
                                             kStatus_Success);

                    loopExtension->loopState = RESOURCE_LOOP_RUNNING;
                    loopExtension->loopStateInitPending = false;

                    if (loopControl->done)
                    {
                        ResourceLoop_completeDone(loop,
                                                  resource,
                                                  controlEvents);
                        return;
                    }

                    if (loopControl->waitTime != 0U)
                    {
                        // Van megadva (startFunc alatt) idozites a loop
                        // elso futasaig. El kell inditani emiatt az idozitest
                        ResourceLoop_scheduleTimer(loop, resource);
                        // Amig fut az idozites, addig nincs mit tenni.
                        return;
                    }

                    // Mivel 0 van megadva idonek, igy azt mondhatja, hogy a
                    // loop futasa idozitve lett.
                    // Ezt jelzi a control objektumban.
                    loopControl->timed = true;
                    break;
                }

                if (loopControl->waitTime != 0U)
                {
                    // Van megadva (startFunc alatt) idozites a loop
                    // elso futasaig. El kell inditani emiatt az idozitest
                    ResourceLoop_scheduleTimer(loop, resource);
                    // Amig fut az idozites, addig nincs mit tenni.
                    return;
                }

                // Mivel 0 van megadva idonek, igy azt mondhatja, hogy a
                // loop futasa idozitve lett.
                // Ezt jelzi a control objektumban.
                loopControl->timed = true;
                break;

            case RESOURCE_LOOP_RUNNING:
                // A manager fele ismetelten megerositjuk a RUNNING allapotot,
                // majd a control.waitTime alapjan utemezzuk a kovetkezo futast.
                ResourceLoop_reportState(resource,
                                         RESOURCE_RUNNING,
                                         kStatus_Success);

                if (loopControl->waitTime != 0U)
                {
                    ResourceLoop_scheduleTimer(loop, resource);
                    return;
                }

                loopControl->timed = true;
                break;

            case RESOURCE_LOOP_STOPPING:
                // A loopFunc a stopRequest flag alapjan befejezheti a munkat.
                // A stopFunc csak akkor fut, amikor a leallitas mar
                // megengedett.
                loopControl->stopRequest = true;
                break;

            case RESOURCE_LOOP_ERROR:
                // Hiba eseten az elozo idoziteseket toroljuk, nehogy a normal
                // feldolgozasi ciklus a hibakezeles kozben is elinduljon.
                loopExtension->loopTimerActive = false;

                if (loopExtension->errorFunc != NULL)
                {
                    // Hiba callback meghivasa
                    loopExtension->errorFunc(
                        resource->callbackData,
                        loopExtension->loopErrorCode);
                }

                // Hibajelzes az eroforras manager fele.
                ResourceLoop_reportState(
                    resource,
                    RESOURCE_ERROR_,
                    loopExtension->loopErrorCode);

                // Ha van megadva errorLoop funkcio, akkor kulon loopban
                // kezeli a hiba feldolgozasat
                if (loopExtension->errorLoopFunc != NULL)
                {
                    if ((controlEvents & RESOURCE_LOOP_EVENT_STOP) != 0U)
                    {
                        // A stop meg folymatban van jelzes.
                        loopExtension->loopStopPending = true;
                    }                   
                    ResourceLoop_changeState(resource,
                                             RESOURCE_LOOP_CLOSING_ERROR);
                    // Taszk ujr afuttatasa, hogy a "Closing" elinduljon...
                    xTaskNotify(loop->taskHandle,
                                RESOURCE_LOOP_NOTIFY_WORK,
                                eSetBits);
                    return;
                }
                break;

            case RESOURCE_LOOP_CLOSING_ERROR:
                // A hibalezaro folyamat az aktualis stopkerest is figyelembe
                // veszi.
                // Az errorLoopStartFunc egyszer inicializalja a lezarast.
                // A tovabbi korokben az errorLoopFunc folytatja a nem blokkolo
                // lezarast.
                loopExtension->loopErrorCleared =
                    loopExtension->loopStopPending;
                loopExtension->loopStopPending = false;
                loopControl->errorClosed = false;
                loopControl->waitTime = 0U;

                if (loopExtension->errorLoopStartFunc != NULL)
                {
                    // Hiba lezarast kezdemenyezo callback hivasa.
                    // Ebben lehet elokesziteni az errorLoop folyamatot, mivel
                    // csak 1-szer fut le.
                    // (Visszatero hibakod elnyelve!)
                    (void)loopExtension->errorLoopStartFunc(
                        resource->callbackData,
                        loopControl);
                }
                break;
        }
    }

    // Az allapothoz tartozo normal futasi resz. Az allapot belepesi kodja fent
    // egyszer futott. Az alabbi ag mar a start/stop/loop callbackek folytatasat
    // kezeli az aktualis esemenyek es control ertekek alapjan.

    if (loopExtension->loopState == RESOURCE_LOOP_WAITING_FOR_START)
    {
        // Varakozas az inditasra. A mar START/STOPPING allapotban levo
        // manager STOP kerese a DONE atmenet utani versenyhelyzetben is
        // visszaigazolast kap, akkor is, ha kozben nem indult ujra a resource.
        if ((controlEvents & RESOURCE_LOOP_EVENT_START) != 0U)
        {
            // A start es stop bit ugyanabban a pillanatban is megerkezhet
            // (peldaul gyors request/release eseten). A START elinditasa elott
            // a STOP kerest a kovetkezo allapotnak megorizzuk.
            if (ResourceLoop_takeStopRequest(loopExtension, controlEvents))
            {
                taskENTER_CRITICAL();
                loopExtension->loopControlEventsAsync |=
                    RESOURCE_LOOP_EVENT_STOP;
                taskEXIT_CRITICAL();
            }
            ResourceLoop_changeState(resource, RESOURCE_LOOP_STARTING);
            xTaskNotify(loop->taskHandle, RESOURCE_LOOP_NOTIFY_WORK, eSetBits);
        }
        else if (ResourceLoop_takeStopRequest(loopExtension, controlEvents))
        {
            ResourceLoop_reportState(resource,
                                     RESOURCE_STOPPED,
                                     kStatus_Success);
            loopExtension->loopTimerActive = false;
            xTaskNotify(loop->taskHandle, RESOURCE_LOOP_NOTIFY_WORK, eSetBits);
            return;
        }
    }
    else if (loopExtension->loopState == RESOURCE_LOOP_STARTING)
    {
        // Inditasi folyamat
        // Inditas kozben is elfogadjuk a stopkerest. Egyebkent a loopFunc
        // folytatja az aszinkron inditast, amig a callback a run flaget be nem
        // allitja.
        if ((controlEvents & RESOURCE_LOOP_EVENT_STOP) != 0U)
        {
            // Az inditasi folyamat alatt az eroforras leallt.
            // Ezt jelzi az eroforras managernek.
            ResourceLoop_changeState(resource, RESOURCE_LOOP_STOPPING);

            // Loop ujra fut majd..
            xTaskNotify(loop->taskHandle, RESOURCE_LOOP_NOTIFY_WORK, eSetBits);
        }
        else if (loopExtension->loopFunc != NULL)
        {
            // Loop futtatasa.
            status = loopExtension->loopFunc(
                resource->callbackData,
                loopControl);
            if (status != kStatus_Success)
            {
                // Hiba a loop futasban. Ugras a hibakezelesrehez...
                goto error;
            }

            if (loopControl->run)
            {
                // A loop alatt a "run" flag be lett allitva, tehat az eroforras
                // elindult.

                // Jelzi az eroforras managernek.
                ResourceLoop_reportState(resource,
                                         RESOURCE_RUNNING,
                                         kStatus_Success);

                loopExtension->loopState = RESOURCE_LOOP_RUNNING;
                loopExtension->loopStateInitPending = false;
                // A loop kovetkezo futasanak idejet beallitja
                ResourceLoop_scheduleTimer(loop, resource);

                // Ha a loop-bol visszaterve a varakozasi ido 0, akkor
                // azonnal fog futni a loop.
                if (loopControl->waitTime == 0U)
                {
                    xTaskNotify(loop->taskHandle,
                                RESOURCE_LOOP_NOTIFY_WORK,
                                eSetBits);
                }
                return;
            }
        }
    }
    else if (loopExtension->loopState == RESOURCE_LOOP_RUNNING)
    {
        // Az eroforras fut. A STOP kerest a callback elott es utan is
        // ellenorizzuk, mert a callback futasa kozben masik task is kerheti
        // a leallitast.
        if (ResourceLoop_takeStopRequest(loopExtension, controlEvents))
        {
            ResourceLoop_changeState(resource, RESOURCE_LOOP_STOPPING);
            xTaskNotify(loop->taskHandle, RESOURCE_LOOP_NOTIFY_WORK, eSetBits);
            return;
        }

        if (loopControl->done)
        {
            ResourceLoop_completeDone(loop, resource, 0U);
            return;
        }

        if (loopExtension->loopFunc != NULL)
        {
            // Loop futtatasa...
            status = loopExtension->loopFunc(resource->callbackData,
                                         loopControl);
            if (status != kStatus_Success)
            {
                // Hiba eseten a hibakezelesi folyamatra ugras...
                goto error;
            }
        }

        // A callback beallithatja a done flaget es kozben STOP keres is
        // erkezhet. A ketto kozul a STOP kap elsobbseget.
        if (loopControl->done)
        {
            ResourceLoop_completeDone(loop, resource, 0U);
            return;
        }

        if (ResourceLoop_takeStopRequest(loopExtension, 0U))
        {
            ResourceLoop_changeState(resource, RESOURCE_LOOP_STOPPING);
            xTaskNotify(loop->taskHandle, RESOURCE_LOOP_NOTIFY_WORK, eSetBits);
            return;
        }
    }
    else if (loopExtension->loopState == RESOURCE_LOOP_STOPPING)
    {
        if (!loopControl->prohibitStop)
        {
            // A leallitas nincs tiltva (visszatatzva). Stop funkcio hivasa
            if (loopExtension->stopFunc != NULL)
            {
                status = loopExtension->stopFunc(resource->callbackData);
                if (status != kStatus_Success)
                {
                    goto error;
                }
            }

            // Jelzes a ResourceManagernek, hogy az eroforras leallt.
            ResourceLoop_reportState(resource,
                                     RESOURCE_STOPPED,
                                     kStatus_Success);

            // Az eroforras ujra az inditasi jelzesre var.
            ResourceLoop_changeState(resource,
                                     RESOURCE_LOOP_WAITING_FOR_START);
            // Ha futna a loop vezerlo timer, akkor azt is leallitja
            loopExtension->loopTimerActive = false;

            // Loop ebresztese ujra, hogy kiertekelodhessen az uj allapot.
            xTaskNotify(loop->taskHandle, RESOURCE_LOOP_NOTIFY_WORK, eSetBits);
            return;
        }

        // <-- ide jut, ha a leallitas vissza van tartvan,  azt a loopban
        // kell vegrehajtani, es majd ha vegzett az eroforras, akkor a
        // prohibitStop torlesevel tudja jelezni.

        if (loopExtension->loopFunc != NULL)
        {
            // Loop futtatasa...
            status = loopExtension->loopFunc(resource->callbackData,
                                         loopControl);
            if (status != kStatus_Success)
            {
                goto error;
            }
        }

        // Ellenorzes, hogy a loop-ban lett e torolve a prohibitStop flag, tehat
        // vegbe ment e a leallitas...
        if (!loopControl->prohibitStop)
        {
            // A flag torlodott. A loop azonnal ujra fut, es ugyanez az
            // allapot ujra kiertekelodik, de mar ugy, hogy a stopFunc
            // le tud futni, es jelezheti a leallast.
            xTaskNotify(loop->taskHandle,
                        RESOURCE_LOOP_NOTIFY_WORK,
                        eSetBits);
        }
    }
    else if (loopExtension->loopState == RESOURCE_LOOP_ERROR)
    {
        // Az eroforras hibaallapotban van.

        if (((controlEvents & RESOURCE_LOOP_EVENT_STOP) != 0U) ||
            loopExtension->loopStopPending)
        {
            loopExtension->loopStopPending = false;
            // Az eroforras leallt; ezt jelezheti a manager fele.
            ResourceLoop_reportState(resource,
                                     RESOURCE_STOPPED,
                                     kStatus_Success);
            // Ujra inditasra fog varni.
            ResourceLoop_changeState(resource,
                                     RESOURCE_LOOP_WAITING_FOR_START);
            // A task ebresztese, hogy az uj allapotot ki tudja ertekelni.
            xTaskNotify(loop->taskHandle, RESOURCE_LOOP_NOTIFY_WORK, eSetBits);
            return;
        }

        // Esemeny szuro torlese
        loopControl->waitedEvents = 0U;
    }
    else if (loopExtension->loopState == RESOURCE_LOOP_CLOSING_ERROR)
    {
        // Eroforras hiba miatti folyamat futtatasa...

        if ((controlEvents & RESOURCE_LOOP_EVENT_STOP) != 0U)
        {
            // Az eroforras managerhez eljutott a hiba miatti jelzes,
            // es az eroforras leallitasi kerelmet is megkapta. Ezt a flaget
            // beallitjuk.
            loopExtension->loopErrorCleared = true;
        }

        // Ha van errorLoop funkcio, akkor azt futtatni kell.
        // Ebabn tortenhet a hibas allapot feldogozasa.
        // Ha vegzett a hibaketelessel, akkor a control objektumban a
        // errorClosed flaggel jelzi. (Erre fog alaphelyzetbe allni ujra
        // az eroforras.)
        if (loopExtension->errorLoopFunc != NULL)
        {
            (void)loopExtension->errorLoopFunc(
                resource->callbackData,
                loopControl);
        }

        loopControl->timed = false;

        if (loopControl->errorClosed &&
            loopExtension->loopErrorCleared)
        {
            // Az errorLoopFunc() alatt az errorClosed jelzes megerkezett,
            // es az eroforras manager is a leallitasi kerelmet kiadta.

            // Jelzes a manager fele, hogy az eroforras leallt.
            ResourceLoop_reportState(resource,
                                     RESOURCE_STOPPED,
                                     kStatus_Success);
            // Ujra az inditasra fog varni.
            ResourceLoop_changeState(resource,
                                     RESOURCE_LOOP_WAITING_FOR_START);

            // Loop ebresztese ujra, hogy kiertekelodhessen az uj allapot.
            xTaskNotify(loop->taskHandle, RESOURCE_LOOP_NOTIFY_WORK, eSetBits);
            return;
        }
    }

    // Ujabb loop futas idozites utemezese
    ResourceLoop_scheduleTimer(loop, resource);
    loopControl->timed = false;
    return;

error:
    // <-- ide ugrik hiba eseten

    // Hibakod megjegyzese
    loopExtension->loopErrorCode = status;

    if (((controlEvents & RESOURCE_LOOP_EVENT_STOP) != 0U) ||
        (loopExtension->loopState == RESOURCE_LOOP_STOPPING))
    {
        // A STOP mar feldolgozasra kerult, mire a stopFunc vagy a stopolas
        // alatti loopFunc hibara futott. A leallitasi szandekot ettol meg kell
        // orizni, kulonben az ERROR allapot nem tud STOPPED-et jelenteni.
        loopExtension->loopStopPending = true;
    }

    // Hiba allapotba lepes
    ResourceLoop_changeState(resource, RESOURCE_LOOP_ERROR);

    // Loop ebresztese, hogy a hiba allapot azonnal feljojjon.
    loopExtension->loopTimerActive = false;
    xTaskNotify(loop->taskHandle, RESOURCE_LOOP_NOTIFY_WORK, eSetBits);
}
//------------------------------------------------------------------------------
