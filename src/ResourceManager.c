//------------------------------------------------------------------------------
// Copyright (c) 2026 [Istvan Suto]
//
// SPDX-License-Identifier: MIT
//------------------------------------------------------------------------------
//------------------------------------------------------------------------------
// Eroforras-kezelest megvalosito modul.
//
// A manager kezeli az eroforrasok eletciklusat, fuggosegeit es a userigenyeket.
// A loophoz rendelt resource callbackjeit a ResourceLoop futtatja.
//
//
// Hasznalati osszefoglalo.
//
// 1. A manager, resource, user es dependency objektumokat a hivo biztositja.
// 2. Eloszor ResourceManager_init() hivando.
// 3. Minden resource-ot Resource_init() hivassal kell inicializalni.
// 4. A resource-ok ResourceManager_addResource() hivasokkal regisztralhatok.
// 5. A usereket ResourceUser_init() hivasokkal kell a resource-hoz rendelni;
//    a user neve diagnosztikai celra jelenik meg a printStatus() kimeneteben.
// 6. Opcionalisan ResourceObserver_init() hivassal esemenyfigyelok
// regisztralhatok.
// 7. A dependency-ket Resource_addDependency() hivasokkal kell felvenni.
// 8. A manager sajat szalaban folyamatosan ResourceManager_run() fut.
// 9. Mas szalak Resource_request() es Resource_release() hivassal
//    jelezhetik a hasznalati igenyt.
// 10. Fejlesztes es konzolos teszteles soran a Resource_start() es
//     Resource_stop() API user nelkul is hasznalhato. Ehhez a
//     RESOURCE_MANAGER_ENABLE_DEVELOPMENT_API=1 build kapcsolo szukseges.
//     Eles alkalmazasi buildben ezt ne engedelyezd.
// 11. Recovery igenylese Resource_recover() hivassal tortenik.
// 12. A resource sajat mukodesi szala Resource_setState() hivassal
//     jelzi az aktualis allapotat es status kodjat.
//
// Az init lepesek sorrendje kotott, es a manager futasa elott kell
// befejezni. A resource-, user- es dependency-listak futas kozben nem
// valtoztathatok. A ResourceManager_run() csak egyetlen szalbol hivhato.
//
// A request/release muveletek csak a user szandekat es a refcountot
// modositjak. A lifecycle aszinkron, az inditas es leallitas callbackeken
// keresztul tortenik. A resource sajat allapotat Resource_setState() hivassal
// jelzi, status polling nincs.
//
// Opcionais startRequestFunc/stopRequestFunc lifecycle callbackek.
// Loop nelkuli resource eseten a callback sajat vegrehajtasi kontextust
// indithat, es Resource_setState() hivassal jelenti az allapotot. Loophoz
// rendelt resource tenyleges callbackjeit a ResourceLoop task futtatja.
// Az onmagatol befejezo resource RESOURCE_DONE allapotot jelezhet;
// ilyenkor a manager nem hiv stop callbacket. A DONE dependency teljesultnek
// szamit, es a resource a befejezes utan elengedi a sajat dependency-it.
//
// A Resource_event_cb_t callback a manager rekurziv mutexenek birtokaban
// fut, ezert abbol a Resource API ujrahivasa engedelyezett. Az errorInfo csak
// RESOURCE_EVENT_ERROR eseten nem NULL, es csak a callback futasa alatt
// ervenyes.
//
// A recovery probalkozasok szama a Resource_init() recoveryMaxAttempts
// parameteretol fugg. A 0 ertek korlatlan probalkozast jelent.
//------------------------------------------------------------------------------

//------------------------------------------------------------------------------
#include "ResourceManager.h"
#include "ResourceLoop.h"
#include <string.h>
#include <assert.h>
#include <stdio.h>

static void Resource_requestRecoveryForDependents(Resource_t* resource,
                                                   Resource_t* faultSource);
static void Resource_cancelRecoveryForDependents(
    Resource_t* resource, Resource_t* faultSource);
static bool Resource_faultDependentsStopped(Resource_t* resource);
static void Resource_dispatchStartRequest(Resource_t* resource);
static void Resource_dispatchStopRequest(Resource_t* resource);
static void ResourceManager_processResourceEvents(ResourceManager_t* manager);

//------------------------------------------------------------------------------
static const char*  ResourceState_strings[]=
{
    [RESOURCE_STOPPED]    = "STOPPED",
    [RESOURCE_STARTING]   = "STARTING",
    [RESOURCE_RUNNING]    = "RUNNING",
    [RESOURCE_STOPPING_]  = "STOPPING",
    [RESOURCE_ERROR_]     = "ERROR",
    [RESOURCE_DONE_]      = "DONE",
    [RESOURCE_WAITING_FOR_DEPENDENCY] = "WAITING_FOR_DEPENDENCY"
};
//------------------------------------------------------------------------------
static const char*  ResourceEvent_strings[]=
{
    [RESOURCE_EVENT_STARTED]   = "STARTED",
    [RESOURCE_EVENT_STOPPED]   = "STOPPED",
    [RESOURCE_EVENT_ERROR]     = "ERROR",
    [RESOURCE_EVENT_RECOVERY]  = "RECOVERY",
    [RESOURCE_EVENT_RECOVERED] = "RECOVERED",
    [RESOURCE_EVENT_DONE]      = "DONE"
};
//------------------------------------------------------------------------------
// Resource allapot szovegge alakitasa nyomkoveteshez.
const char*  Resource_stateTosStr(ResourceState_t state)
{
    if (state >= ARRAY_SIZE(ResourceState_strings)) {
        return "???";
    }
    return ResourceState_strings[state];
}
//------------------------------------------------------------------------------
// Resource esemeny szovegge alakitasa nyomkoveteshez.
const char*  Resource_eventTosStr(ResourceEvent_t event)
{
    if (event >= ARRAY_SIZE(ResourceEvent_strings)) {
        return "???";
    }
    return ResourceEvent_strings[event];
}
//------------------------------------------------------------------------------
// A manager mutexenek felvetele.
static inline void ResourceManager_lock(ResourceManager_t* manager)
{
    xSemaphoreTakeRecursive(manager->mutex, portMAX_DELAY);
}
//------------------------------------------------------------------------------
// A manager mutexenek feloldasa.
static void ResourceManager_unlock(ResourceManager_t* manager)
{
    xSemaphoreGiveRecursive(manager->mutex);
}
//------------------------------------------------------------------------------
// Resource felrakasa a pending listara.
// A manager mutexet a hivo tartja.
static void ResourceManager_queueResource(Resource_t* resource)
{
    ResourceManager_t* manager = resource->manager;

    if (resource->flags.pending)
    {
        // Mar a lista eleme. Nem szabad ujra hozzaadni!
        return;
    }

    resource->flags.pending = true;
    resource->pendingNext = manager->pendingResources;
    manager->pendingResources = resource;

    // A pending lista tartalmazza a feldolgozando munkat, a binaris szemafor
    // csak ebresztesre szolgal. Ha a szemafor mar adott allapotban van, az
    // ujabb give elhagyhato; a pending lista ettol fuggetlenul ervenyes.
    (void)xSemaphoreGive(manager->wakeup);
}
//------------------------------------------------------------------------------
// Resource kivetele a pending listabol.
// A manager mutexet a hivo tartja.
static
Resource_t* ResourceManager_popPendingResource(ResourceManager_t* manager)
{
    Resource_t* resource = manager->pendingResources;

    if (resource == NULL)
    {
        return NULL;
    }

    manager->pendingResources = resource->pendingNext;
    resource->pendingNext = NULL;
    resource->flags.pending = false;

    return resource;
}
//------------------------------------------------------------------------------

// Inditasi kerelem tovabbitasa a callbacknek es a loopnak.
static inline void Resource_dispatchStartRequest(Resource_t* resource)
{
    ResourceLoopExtension_t* loopExtension = resource->loopExtension;

    if (resource->startRequestFunc != NULL)
    {
        status_t status = resource->startRequestFunc(
            resource, resource->callbackData);

        if (status != kStatus_Success)
        {
            ResourceManager_lock(resource->manager);
            resource->flags.lifecycleActive = false;
            ResourceManager_unlock(resource->manager);
            (void)Resource_reportError(resource, resource, status,
                                       RESOURCE_RECOVERY_OVERRIDE_DEFAULT);
            return;
        }
    }
    else if (loopExtension == NULL)
    {
        // Callback nelkuli, loopmentes resource azonnal elindultnak szamit.
        Resource_setState(resource, RESOURCE_RUNNING, kStatus_Success);
    }

    if ((loopExtension != NULL) &&
        (loopExtension->loop != NULL))
    {
        ResourceLoop_setStartRequest(resource);
    }
}
//------------------------------------------------------------------------------
// Leallitasi kerelem tovabbitasa a callbacknek es a loopnak.
static inline void Resource_dispatchStopRequest(Resource_t* resource)
{
    ResourceLoopExtension_t* loopExtension = resource->loopExtension;

    if (resource->stopRequestFunc != NULL)
    {
        status_t status = resource->stopRequestFunc(
            resource, resource->callbackData);

        if (status != kStatus_Success)
        {
            ResourceManager_lock(resource->manager);
            resource->flags.stopRequestFailed = true;
            ResourceManager_unlock(resource->manager);
            (void)Resource_reportError(resource, resource, status,
                                       RESOURCE_RECOVERY_OVERRIDE_DEFAULT);
            // A loop sajat stop kerelet ettol fuggetlenul megkapja a
            // leallitasi jelzest, hogy a hibas request callback ne hagyja
            // futva a kooperativ eroforrast.
        }
    }

    if ((loopExtension != NULL) &&
        (loopExtension->loop != NULL))
    {
        ResourceLoop_setStopRequest(resource);
    }
}
//------------------------------------------------------------------------------
// Loop nelkuli resource-ok esemenyeinek feldolgozasa a manager taskjaban.
// A manager listajan haladunk vegig, de loopos resource-hoz nem nyulunk, mert
// annak esemenymaszkjat kizarolag a ResourceLoop task fogyaszthatja el.
static void ResourceManager_processResourceEvents(ResourceManager_t* manager)
{
    Resource_t* resource;

    for (resource = manager->resources; resource != NULL;
         resource = resource->next)
    {
        ResourceLoopExtension_t* loopExtension = resource->loopExtension;
        uint64_t events;
        Resource_event_fn_t callback;
        void* callbackData;

        // Loophoz rendelt resource esemenyeit a ResourceLoop olvassa ki.
        if ((loopExtension != NULL) &&
            (loopExtension->loop != NULL))
        {
            continue;
        }

        // Atvesszuk az esemeny pillanatnyi halmazat, es kiuritjuk a tarolot.
        // A kritikus szakasz utan erkezo uj esemeny mar a kovetkezo korhoz
        // tartozik.
        taskENTER_CRITICAL();
        events = resource->inputEventsAsync;
        resource->inputEventsAsync = 0U;
        taskEXIT_CRITICAL();

        if (events == 0U)
        {
            continue;
        }

        // A callback es a kontextus lokalis masolatot kap. A callback nem a
        // manager mutexenek birtokaban fut, igy nem tartja fel az allapotgepek
        // feldolgozasat.
        callback = resource->eventFunc;
        callbackData = resource->callbackData;
        if (callback != NULL)
        {
            callback(resource, events, callbackData);
        }
    }
}
//------------------------------------------------------------------------------
// Hibainformacio frissitese es a fuggok ujraellenorzese.
// A manager mutexet a hivo tartja.
static void Resource_clearCurrentErrorInfo(Resource_t* resource)
{
    memset(&resource->errorInfo, 0, sizeof(resource->errorInfo));
    resource->errorInfo.status = kStatus_Success;
}
//------------------------------------------------------------------------------
// Hiba jelzo struktura inicializalasa, es kitoltese
static void Resource_initErrorInfo(ResourceErrorInfo_t* errorInfo,
                                   status_t status,
                                   Resource_t* faultSource,
                                   Resource_t* reportedBy,
                                   Resource_t* failedDependency,
                                   ResourceErrorReason_t reason,
                                   ResourceRecoveryOverride_t recoveryOverride)
{
    memset(errorInfo, 0, sizeof(*errorInfo));
    errorInfo->status = status;
    errorInfo->resource = faultSource;
    errorInfo->faultSource = faultSource;
    errorInfo->reportedBy = reportedBy;
    errorInfo->failedDependency = failedDependency;
    errorInfo->reason = reason;
    errorInfo->recoveryOverride = recoveryOverride;
}
//------------------------------------------------------------------------------
// Hibainformacio frissitese es a fuggok ujraellenorzese.
// A manager mutexet a hivo tartja.
// Az elso hiba adatai egy hibafolyamatban nem cserelodnek le, viszont a
// recovery felulbiralas csak erosebb iranyba valtozhat.
static void Resource_copyErrorInfo(Resource_t* resource,
                                   const ResourceErrorInfo_t* errorInfo)
{
    ResourceDependency_t* dep;

    if (errorInfo->status == kStatus_Success)
    {
        // Dependency-alapu varakozas megszunhet, ha a dependency helyreallt.
        // A sajat vagy jelentett hiba diagnosztikajat csak a sikeres RUNNING
        // atmenet zarja le.
        if (resource->errorInfo.reason ==
            RESOURCE_ERROR_REASON_DEPENDENCY_FAULT)
        {
            Resource_clearCurrentErrorInfo(resource);
            resource->flags.faultProcessActive = false;
            resource->recoveryOverride = RESOURCE_RECOVERY_OVERRIDE_DEFAULT;
        }
        return;
    }

    if (resource->flags.faultProcessActive)
    {
        if (resource->errorInfo.status == kStatus_Success)
        {
            resource->errorInfo = resource->lastErrorInfo;
        }
        // A felulbiralas a hibafolyamathoz tartozik, nem orok ervenyu.
        // Egy fuggetlen hibaforras nem erositheti a mar aktiv hibafolyamat
        // felulbiralasat.
        // Igy a felso szintu DISABLED kaszkad nem tiltja le a sajat hiba AUTO
        // recoveryjet.
        // Azonos gyokeru hibaforras eseten a felulbiralas tovabb erosodhet.
        if ((errorInfo->faultSource == resource->errorInfo.faultSource) &&
            (errorInfo->recoveryOverride > resource->recoveryOverride))
        {
            resource->recoveryOverride = errorInfo->recoveryOverride;
            resource->errorInfo.recoveryOverride = resource->recoveryOverride;
            resource->lastErrorInfo.recoveryOverride =
                                                    resource->recoveryOverride;
        }
        return;
    }

    resource->lastErrorStatus = errorInfo->status;
    resource->errorInfo = *errorInfo;
    resource->lastErrorInfo = *errorInfo;
    resource->flags.faultProcessActive = true;
    resource->recoveryOverride = errorInfo->recoveryOverride;
    resource->flags.recoveryFailed = false;
    resource->flags.errorEventSent = false;

    for (dep = resource->dependents; dep != NULL; dep = dep->nextDependent)
    {
        // A PROPAGATE kapcsolat eltarolja a hibat a dependent feldolgozasaig,
        // akkor is, ha a dependency kozben gyorsan helyreall.
        if ((dep->errorStrategy == RESOURCE_DEP_PROPAGATE) &&
            dep->internalUser.requested)
        {
            ResourceErrorInfo_t dependentErrorInfo = *errorInfo;
            dependentErrorInfo.reason = RESOURCE_ERROR_REASON_DEPENDENCY_FAULT;
            dependentErrorInfo.failedDependency = resource;

            if (!dep->pendingError)
            {
                dep->pendingErrorInfo = dependentErrorInfo;
            }
            dep->pendingError = true;
        }

        ResourceManager_queueResource(dep->dependent);
    }
}
//------------------------------------------------------------------------------
// Dependency graph bejaras.
// A hibajelentesnel a reporter es a faultSource kapcsolat ellenorzesere
// is szukseg van.
// A ciklusellenorzes csak a hozzaadas helyen, forditasi kapcsoloval aktiv.
// Rekurzivan megkeresi, hogy target elerheto-e startbol.
static bool Resource_graphContains(Resource_t* start, Resource_t* target)
{
    ResourceDependency_t* dep;

    if (start == NULL)
    {
        return false;
    }

    if (start == target)
    {
        return true;
    }

    // Minden dependency agban folytatjuk a keresest.
    for (dep = start->dependencies; dep != NULL; dep = dep->next)
    {
        if (Resource_graphContains(dep->resource, target))
        {
            return true;
        }
    }

    return false;
}
//------------------------------------------------------------------------------
// Ellenorzi, hogy a user objektum mar szerepel-e valamelyik
#if defined(RESOURCE_MANAGER_ENABLE_REGISTRATION_DUPLICATE_CHECK) && \
    (RESOURCE_MANAGER_ENABLE_REGISTRATION_DUPLICATE_CHECK != 0)
//------------------------------------------------------------------------------
// Megkeresi, hogy a user objektumot regisztraltak-e mar valamelyik
// resource-nal. Ez a teljes konfiguracio bejarasaval jarhat.
static bool ResourceUser_inUse(ResourceManager_t* manager,
                               ResourceUser_t* user)
{
    Resource_t* resource = manager->resources;
    for (; resource != NULL; resource = resource->next)
    {
        ResourceUser_t* current= resource->users;
        for (; current != NULL; current = current->next)
        {
            if (current == user)
            {
                return true;
            }
        }
    }

    return false;
}
//------------------------------------------------------------------------------
// Megkeresi, hogy az observer objektumot regisztraltak-e mar.
static bool ResourceObserver_inUse(ResourceManager_t* manager,
                                   ResourceObserver_t* observer)
{
    Resource_t* resource = manager->resources;

    for (; resource != NULL; resource = resource->next)
    {
        ResourceObserver_t* current = resource->observers;
        for (; current != NULL; current = current->next)
        {
            if (current == observer)
            {
                return true;
            }
        }
    }

    return false;
}
//------------------------------------------------------------------------------
// Megkeresi, hogy a dependency objektumot hasznaltak-e mar kapcsolatban.
static bool Resource_dependencyInUse(ResourceManager_t* manager,
                                     ResourceDependency_t* dependency)
{

    Resource_t* resource= manager->resources;
    for (; resource != NULL; resource = resource->next)
    {
        ResourceDependency_t* current= resource->dependencies;
        for (; current != NULL; current = current->next)
        {
            if (current == dependency)
            {
                return true;
            }
        }
    }

    return false;
}
#endif
//------------------------------------------------------------------------------
// User-esemeny kuldese a regisztralt callback meghivasaval.
// A manager mutexet a hivo tartja.
// A lista a manager futasa alatt nem valtozik.
// (A callback meghivhatja a Resource API-t, mert a mutex rekurziv.)
static void Resource_notifyEvent(Resource_t* resource,
                                 ResourceEvent_t event,
                                 const ResourceErrorInfo_t* errorInfo)
{
    ResourceErrorInfo_t errorInfoCopy;
    const ResourceErrorInfo_t* eventErrorInfo = NULL;

    if (errorInfo != NULL)
    {
        errorInfoCopy = *errorInfo;
        eventErrorInfo = &errorInfoCopy;
    }

    // Resource-szintu esemenyfigyeloket egyszer ertesitjuk esemenyenkent,
    // fuggetlenul az aktiv userek szamatol. A lista a manager futasa alatt
    // nem valtozik. A callbackek a manager rekurziv mutexenek birtokaban
    // futnak.
    ResourceObserver_t* observer;
    for (observer = resource->observers;
         observer != NULL;
         observer = observer->next)
    {
        if (observer->eventFunc != NULL)
        {
            observer->eventFunc(resource, event, eventErrorInfo,
                               observer->callbackData);
        }
    }

    // Vegigjarjuk az eroforras osszes useret.
    ResourceUser_t* user;
    for (user = resource->users; user != NULL; user = user->next)
    {
        // Az inaktiv user nem kap esemenyjelzest.
        if (!user->requested)
        {
            continue;
        }

        // A STARTED/RECOVERED esemeny egy ujonnan igenylo user keszenleti
        // visszajelzeset is kielegiti, ha az esemeny kozben kerte a resource-t.
        if ((event == RESOURCE_EVENT_STARTED) ||
            (event == RESOURCE_EVENT_RECOVERED))
        {
            user->startedEventPending = false;
        }
        else if (event == RESOURCE_EVENT_DONE)
        {
            user->doneEventPending = false;
        }

        // Callback hivasa...
        if (user->eventFunc != NULL)
        {
            user->eventFunc(user, event, eventErrorInfo, user->callbackData);
        }
    }
}
//------------------------------------------------------------------------------
// Egy mar RUNNING resource-t ujonnan igenylo user keszenleti visszajelzese.
// A manager mutexet a hivo tartja. A usert nem szabad ujra ertesiteni, ha
// kozben a resource allapota megvaltozott.
static void Resource_notifyPendingStartedUsers(Resource_t* resource)
{
    ResourceUser_t* user;

    for (user = resource->users; user != NULL; user = user->next)
    {
        if (!user->requested || !user->startedEventPending)
        {
            continue;
        }

        // A flaget a callback elott toroljuk, hogy egy reentrans callback
        // ne kapja meg ugyanazt a vissza jelzest megegyszer.
        user->startedEventPending = false;

        if ((resource->state != RESOURCE_RUNNING) ||
            (resource->flags.runningEventSent == false))
        {
            // Ha mar nem RUNNING, a keszenleti visszajelzest elhalasztjuk.
            user->startedEventPending = true;
            return;
        }

        if (user->eventFunc != NULL)
        {
            user->eventFunc(user, RESOURCE_EVENT_STARTED, NULL,
                           user->callbackData);
        }

        // A callback reentrans lehet es megvaltoztathatja az allapotot.
        if (resource->state != RESOURCE_RUNNING)
        {
            return;
        }
    }
}
//------------------------------------------------------------------------------
// DONE allapotban erkezo uj request DONE esemenyenek kuldese.
// A manager mutexet a hivo tartja. A flaget callback elott toroljuk, hogy
// reentrans request ne kapja meg ugyanazt az esemenyt ketszer.
static void Resource_notifyPendingDoneUsers(Resource_t* resource)
{
    ResourceUser_t* user;

    if ((resource->state != RESOURCE_DONE_) || !resource->flags.doneEventSent)
    {
        return;
    }

    for (user = resource->users; user != NULL; user = user->next)
    {
        if (!user->requested || !user->doneEventPending)
        {
            continue;
        }

        user->doneEventPending = false;

        if (user->eventFunc != NULL)
        {
            user->eventFunc(user, RESOURCE_EVENT_DONE, NULL,
                           user->callbackData);
        }

        // A callback reentrans lehet es megvaltoztathatja a resource allapotat.
        if (resource->state != RESOURCE_DONE_)
        {
            return;
        }
    }
}
//------------------------------------------------------------------------------
// Recovery igenylesek vizsgalata
static bool Resource_hasRecoveryRequest(Resource_t* resource)
{
    // Vegigjarjuk az eroforras osszes useret.
    ResourceUser_t* user;
    for (user = resource->users; user != NULL; user = user->next)
    {
        if ((user->requested) && (user->recoveryRequested))
        {
            return true;
        }
    }
    return false;
}
//------------------------------------------------------------------------------
// Recovery igenylesek torlese
static void Resource_clearRecoveryRequests(Resource_t* resource)
{
    // Vegigjarjuk az eroforras osszes useret.
    ResourceUser_t* user;
    for (user = resource->users; user != NULL; user = user->next)
    {
        user->recoveryRequested = false;
    }
}
//------------------------------------------------------------------------------
// Automatikus recovery keresese egy mar rogzitett hiba miatt.
// A hiba lehet a resource sajat hibaja vagy egy dependency-bol propagalt hiba.
// A manager mutexet a hivo tartja. A hivo gondoskodik a queue-olasrol.
static bool ResourceUser_requestAutoRecoveryOnError(ResourceUser_t* user)
{
    Resource_t* resource = user->resource;

    // Kizaro okok ellenorzese...
    if (!user->requested ||
        !user->autoRecovery ||
        (resource->recoveryOverride == RESOURCE_RECOVERY_OVERRIDE_DISABLED) ||
        (resource->errorInfo.status == kStatus_Success) ||
        (resource->errorInfo.resource == NULL) ||
        resource->flags.recoveryActive ||
        ((resource->recoveryMaxAttempts != 0) &&
         (resource->recoveryAttempts >= resource->recoveryMaxAttempts)))
    {
        return false;
    }

    // Jelzes az userben, hogy recovery kerelem van
    user->recoveryRequested = true;
    resource->flags.recoveryPending = true;
    resource->flags.recoveryCascadePending = true;

    // A recovery okat az errorInfo alapjan az eroforrasban is beallitja.
    if (resource->errorInfo.reason == RESOURCE_ERROR_REASON_REPORTED_FAULT)
    {
        resource->recoveryReason =
            RESOURCE_RECOVERY_REASON_HIGHER_LAYER_REQUEST;
    } else
    if (resource->errorInfo.reason == RESOURCE_ERROR_REASON_DEPENDENCY_FAULT)
    {
        resource->recoveryReason = RESOURCE_RECOVERY_REASON_DEPENDENCY;
    } else
    {
        resource->recoveryReason = RESOURCE_RECOVERY_REASON_SELF_FAULT;
    }

    return true;
}
//------------------------------------------------------------------------------
// Dependencia aktivizalasa
// Ha a resource igenyelt, minden dependency belso userjet igenyelni kell.
// Ez csak egy bool request, tehat ha egy resource-nak ket kulso usere van,
// attol meg csak egy belso dependency user kell.
// A dependency sajat refcountja azt szamolja, hany kulonallo resource
// igenyli ot.
static void Resource_requestDependencies(Resource_t* resource)
{
    // Vegig afuggosegek lancolt listajan...
    ResourceDependency_t* dep;
    for (dep = resource->dependencies; dep != NULL; dep = dep->next)
    {
        if (!dep->internalUser.requested)
        {
            dep->pendingError = false;
            dep->pendingErrorInfo.status = kStatus_Success;
            dep->pendingErrorInfo.resource = NULL;
            dep->internalUser.requested = true;
            dep->resource->refCount++;
            ResourceManager_queueResource(dep->resource);
        }
    }
}
//------------------------------------------------------------------------------
// Dependencia elengedese
static void Resource_releaseDependencies(Resource_t* resource)
{
    // Vegig a fuggosegek lancolt listajan...
    ResourceDependency_t* dep;
    for (dep = resource->dependencies; dep != NULL; dep = dep->next)
    {
        if (dep->internalUser.requested)
        {
            // Az adott dependenciat valoban keri az eroforras

            dep->internalUser.requested = false;
            dep->pendingError = false;
            dep->pendingErrorInfo.status = kStatus_Success;
            dep->pendingErrorInfo.resource = NULL;

            assert(dep->resource->refCount > 0);
            // A dependencia referencia szamlaloja csokken
            dep->resource->refCount--;

            dep->internalUser.recoveryRequested = false;
            ResourceManager_queueResource(dep->resource);
        }
    }
}
//------------------------------------------------------------------------------
// Dependencia allapot ellenorzese
// Visszater:
//  igaz  = minden szukseges dependency megfelelo
//  hamis = valamelyik dependency miatt varni kell
// Az errorFound igaz lesz, ha valamelyik dependency ERROR allapotban van.
static bool Resource_checkDependencies(Resource_t* resource,
                                       bool* errorFound,
                                       bool* recoveryRequested,
                                       ResourceErrorInfo_t* errorInfo)
{
    bool dependenciesReady = true;
    ResourceDependency_t* dep;

    *errorFound = false;
    *recoveryRequested = false;
    if (errorInfo != NULL)
    {
        memset(errorInfo, 0, sizeof(*errorInfo));
        errorInfo->status = kStatus_Success;
        errorInfo->recoveryOverride = RESOURCE_RECOVERY_OVERRIDE_DEFAULT;
    }

    // Minden dependency-t meg kell vizsgalni, mert a dontes nem fugghet
    // a dependency-k listaban elfoglalt sorrendjetol.
    for (dep = resource->dependencies; dep != NULL; dep = dep->next)
    {
        Resource_t* dependencyResource = dep->resource;

        // A pending hiba akkor is szamit, ha a dependency kozben mar
        // RUNNING allapotba tert vissza. Egy feldolgozasi korben fogyasztjuk
        // el, es ujra sorba tesszuk a dependentet, hogy recovery kozben is
        // legyen lehetoseg a helyreallast kulon ellenorizni.
        if ((dep->errorStrategy == RESOURCE_DEP_PROPAGATE) &&
            dep->pendingError)
        {
            *errorFound = true;
            dependenciesReady = false;
            if ((errorInfo != NULL) && (errorInfo->resource == NULL))
            {
                *errorInfo = dep->pendingErrorInfo;
            }
            dep->pendingError = false;
            dep->pendingErrorInfo.status = kStatus_Success;
            dep->pendingErrorInfo.resource = NULL;
            ResourceManager_queueResource(resource);
        }

        if ((dependencyResource->state == RESOURCE_RUNNING) ||
            (dependencyResource->state == RESOURCE_DONE_))
        {
            continue;
        }

        if ((dependencyResource->state == RESOURCE_ERROR_) ||
            (((dependencyResource->state == RESOURCE_WAITING_FOR_DEPENDENCY) ||
              (dependencyResource->state == RESOURCE_STOPPING_) ||
              (dependencyResource->state == RESOURCE_STOPPED)) &&
             (dependencyResource->errorInfo.status != kStatus_Success) &&
             (dependencyResource->errorInfo.resource != NULL)))
        {
            switch (dep->errorStrategy)
            {
                case RESOURCE_DEP_IGNORE:
                    // Az IGNORE kifejezetten azt jelenti, hogy az ERROR
                    // allapotu dependency nem blokkolja a dependentet.
                    // Soron kovetkezo vizsgalata...
                    continue;

                case RESOURCE_DEP_PROPAGATE:
                    // Jelezni kell az igenylonek, hogy a dependencia hiba
                    // allapotban van, 
                    *errorFound = true;
                    dependenciesReady = false;
                    if ((errorInfo != NULL) &&
                        (errorInfo->resource == NULL))
                    {
                        *errorInfo = dependencyResource->errorInfo;

                        errorInfo->reason =
                                        RESOURCE_ERROR_REASON_DEPENDENCY_FAULT;

                        errorInfo->failedDependency = dependencyResource;

                        if (errorInfo->status == kStatus_Success)
                        {
                            errorInfo->status = kStatus_Fail;
                            errorInfo->resource = dependencyResource;
                            errorInfo->faultSource = dependencyResource;
                            errorInfo->reportedBy = dependencyResource;
                        }
                    }
                    break;

                case RESOURCE_DEP_AUTO_RECOVER:
                    // Ha az AUTO_RECOVER dependency mar elerte a recovery
                    // probalkozasok maximumat, akkor a hiba propagalodik.
                    if ((dependencyResource->recoveryMaxAttempts != 0) &&
                        (dependencyResource->recoveryAttempts >=
                         dependencyResource->recoveryMaxAttempts))
                    {
                        *errorFound = true;
                    }
                    else
                    {
                        *recoveryRequested = true;
                    }

                    if ((errorInfo != NULL) &&
                        (errorInfo->resource == NULL))
                    {
                        *errorInfo = dependencyResource->errorInfo;

                        errorInfo->reason =
                                    RESOURCE_ERROR_REASON_DEPENDENCY_FAULT;

                        errorInfo->failedDependency = dependencyResource;

                        if (errorInfo->status == kStatus_Success)
                        {
                            errorInfo->status = kStatus_Fail;
                            errorInfo->resource = dependencyResource;
                            errorInfo->faultSource = dependencyResource;
                            errorInfo->reportedBy = dependencyResource;
                        }
                    }
                    dependenciesReady = false;
                    break;

                default:
                    *errorFound = true;
                    dependenciesReady = false;
                    if ((errorInfo != NULL) && (errorInfo->resource == NULL))
                    {
                        *errorInfo = dependencyResource->errorInfo;

                        errorInfo->reason =
                                        RESOURCE_ERROR_REASON_DEPENDENCY_FAULT;

                        errorInfo->failedDependency = dependencyResource;

                        if (errorInfo->status == kStatus_Success)
                        {
                            errorInfo->status = kStatus_Fail;
                            errorInfo->resource = dependencyResource;
                            errorInfo->faultSource = dependencyResource;
                            errorInfo->reportedBy = dependencyResource;
                        }
                    }
                    break;
            }

            continue;
        }

        // STARTING, hiba nelkuli STOPPING, WAITING vagy STOPPED
        // allapotban egyszeruen varni kell a dependency-re.
        dependenciesReady = false;
    }

    // Minden dependency feldolgozasa megtortent. A ket eredmeny
    // fuggetlen egymastol: lehet egyszerre PROPAGATE hiba es
    // AUTO_RECOVER igeny is.
    return dependenciesReady;
}
//------------------------------------------------------------------------------
// Recovery kerelem kuldese az AUTO_RECOVER dependency-knek.
// A manager mutexet a hivo tartja.
static void Resource_requestDependencyRecovery(Resource_t* resource)
{
    ResourceDependency_t* dep;

    // Vegig a fuggosegek lancolt listajan...
    for (dep = resource->dependencies; dep != NULL; dep = dep->next)
    {
        if ((dep->resource->state == RESOURCE_ERROR_) &&
            (dep->errorStrategy == RESOURCE_DEP_AUTO_RECOVER))
        {
            // A dependency belso userje mar igenyli a resource-t, ezert
            // ugyanazon recovery mechanizmuson keresztul kezelheto.
            dep->internalUser.recoveryRequested = true;
            dep->resource->flags.recoveryCascadePending = true;
            ResourceManager_queueResource(dep->resource);
        }
    }
}
//------------------------------------------------------------------------------
// A dependent sajat userenek recovery-kerese a PROPAGATE kapcsolat menten
// a hibas prerequisite-ekre is kiterjed. Recovery-keres nelkul a PROPAGATE
// csak a hibat tovabbitja, nem indit el onmagaban automatikus recoveryt.
// A manager mutexet a hivo tartja.
static void Resource_requestPropagatedDependencyRecovery(Resource_t* resource)
{
    ResourceDependency_t* dep;
    bool blockedByLimit = false;

    if (!resource->flags.recoveryPending ||
        !resource->flags.recoveryCascadePending)
    {
        return;
    }

    // Elobb megnezzuk, hogy barmely hibas PROPAGATE dependency elerte-e a
    // sajat recovery limitjet. Ilyenkor az automatikus lanc nem lepheti at
    // a dependency sajat korlatjat.
    for (dep = resource->dependencies; dep != NULL; dep = dep->next)
    {
        Resource_t* dependencyResource = dep->resource;

        if ((dep->errorStrategy != RESOURCE_DEP_PROPAGATE) ||
            !dep->internalUser.requested ||
            (dependencyResource->errorInfo.status == kStatus_Success) ||
            (dependencyResource->errorInfo.resource == NULL))
        {
            continue;
        }

        if ((dependencyResource->state == RESOURCE_ERROR_) ||
            (dependencyResource->state == RESOURCE_WAITING_FOR_DEPENDENCY) ||
            (dependencyResource->state == RESOURCE_STOPPING_) ||
            (dependencyResource->state == RESOURCE_STOPPED))
        {
            if ((dependencyResource->recoveryMaxAttempts != 0) &&
                (dependencyResource->recoveryAttempts >=
                                    dependencyResource->recoveryMaxAttempts))
            {
                blockedByLimit = true;
            }
        }
    }

    if (blockedByLimit)
    {
        resource->flags.recoveryPending = false;
        resource->flags.recoveryCascadePending = false;
        Resource_clearRecoveryRequests(resource);
        return;
    }

    for (dep = resource->dependencies; dep != NULL; dep = dep->next)
    {
        Resource_t* dependencyResource = dep->resource;

        if ((dep->errorStrategy != RESOURCE_DEP_PROPAGATE) ||
            !dep->internalUser.requested ||
            (dependencyResource->errorInfo.status == kStatus_Success) ||
            (dependencyResource->errorInfo.resource == NULL))
        {
            continue;
        }

        if ((dependencyResource->state == RESOURCE_ERROR_) ||
            (dependencyResource->state == RESOURCE_WAITING_FOR_DEPENDENCY) ||
            (dependencyResource->state == RESOURCE_STOPPING_) ||
            (dependencyResource->state == RESOURCE_STOPPED))
        {
            dep->internalUser.recoveryRequested = true;
            dependencyResource->flags.recoveryCascadePending = true;
            dependencyResource->recoveryReason =
                                        RESOURCE_RECOVERY_REASON_DEPENDENCY;
            ResourceManager_queueResource(dependencyResource);
        }
    }
}
//------------------------------------------------------------------------------
// Recovery probalkozas elinditasa.
// A manager mutexet a hivo tartja.
// Igaz ertekkel ter vissza, ha a probalkozas elindithato.
static bool Resource_beginRecovery(Resource_t* resource, bool notifyEvent)
{
    if ((resource->recoveryMaxAttempts != 0) &&
        (resource->recoveryAttempts >= resource->recoveryMaxAttempts))
    {
        // Nincs lehetoseg (tobb) recoveryre.
        resource->flags.recoveryPending = false;
        resource->flags.recoveryCascadePending = false;
        resource->flags.recoveryFailed = true;
        Resource_clearRecoveryRequests(resource);
        return false;
    }

    // Probalkozas szam novekszik. A user- es belso AUTO_RECOVER kerest
    // egy probalkozasra fogyasztjuk el; kesobbi lifecycle-esemeny nem
    // hasznalhatja fel ujra ugyanazt a kerest. A recoveryPending ettol
    // kulon marad aktiv a probalkozas sikeres lezarasaig.
    resource->recoveryAttempts++;
    resource->flags.recoveryActive = true;
    Resource_clearRecoveryRequests(resource);

    if (notifyEvent)
    {
        // Elo van irva a jelzes az userek fele.
        Resource_notifyEvent(resource, RESOURCE_EVENT_RECOVERY, NULL);
    }
    return true;
}
//------------------------------------------------------------------------------
// ERROR atmenet mellekhatasai.
// A manager mutexet a hivo tartja.
// Akkor is alkalmazhato, ha a lifecycle mar STOPPING allapotban van, es
// a hiba miatt nem szabad masodik stop callbacket inditani.
static ResourceRecoveryReason_t Resource_reasonForError(
                                        const ResourceErrorInfo_t* errorInfo)
{
    if (errorInfo->reason == RESOURCE_ERROR_REASON_REPORTED_FAULT)
    {
        return RESOURCE_RECOVERY_REASON_HIGHER_LAYER_REQUEST;
    }

    if (errorInfo->reason == RESOURCE_ERROR_REASON_DEPENDENCY_FAULT)
    {
        return RESOURCE_RECOVERY_REASON_DEPENDENCY;
    }

    return RESOURCE_RECOVERY_REASON_SELF_FAULT;
}
//------------------------------------------------------------------------------
// ERROR atmenet mellekhatasai.
// A manager mutexet a hivo tartja.
static void Resource_applyErrorSideEffects(Resource_t* resource,
                                           bool recoveryWasActive)
{
    ResourceUser_t* user;

    resource->flags.runningEventSent = false;
    resource->flags.restartAfterStop = false;

    if (recoveryWasActive)
    {
        resource->flags.recoveryActive = false;
        if ((resource->recoveryMaxAttempts == 0) ||
            (resource->recoveryAttempts < resource->recoveryMaxAttempts))
        {
            resource->flags.recoveryPending = true;
        }
        else
        {
            resource->flags.recoveryPending = false;
            resource->flags.recoveryCascadePending = false;
            resource->flags.recoveryFailed = true;
            Resource_clearRecoveryRequests(resource);
        }
    }

    if ((resource->errorInfo.status == kStatus_Success) ||
        (resource->errorInfo.faultSource == NULL))
    {
        return;
    }

    if (resource->recoveryOverride == RESOURCE_RECOVERY_OVERRIDE_FORCED)
    {
        if (!resource->flags.recoveryActive &&
            ((resource->recoveryMaxAttempts == 0) ||
             (resource->recoveryAttempts < resource->recoveryMaxAttempts)))
        {
            resource->flags.recoveryPending = true;
            resource->recoveryReason =
                                Resource_reasonForError(&resource->errorInfo);
            resource->flags.recoveryFailed = false;
        }
    }
    else if (resource->recoveryOverride == RESOURCE_RECOVERY_OVERRIDE_DISABLED)
    {
        if (!resource->flags.recoveryActive)
        {
            resource->flags.recoveryPending = false;
            resource->flags.recoveryCascadePending = false;
            Resource_clearRecoveryRequests(resource);
        }
    }
    else if ((resource->recoveryPolicy == RESOURCE_RECOVERY_POLICY_AUTO) &&
             (resource->errorInfo.faultSource == resource))
    {
        if (!resource->flags.recoveryActive &&
            ((resource->recoveryMaxAttempts == 0) ||
             (resource->recoveryAttempts < resource->recoveryMaxAttempts)))
        {
            resource->flags.recoveryPending = true;
            resource->recoveryReason =
                                Resource_reasonForError(&resource->errorInfo);
            resource->flags.recoveryFailed = false;
        }
    }

    // A user szintu autoRecovery kulon, explicit beallitas marad a
    // kompatibilitas miatt.
    // A DISABLED felulbiralas ezt letiltja.
    if (resource->recoveryOverride != RESOURCE_RECOVERY_OVERRIDE_DISABLED)
    {
        for (user = resource->users; user != NULL; user = user->next)
        {
            (void)ResourceUser_requestAutoRecoveryOnError(user);
        }
    }

    // Ha maga a hibaforras all helyreallas alatt, minden aktiv PROPAGATE
    // fuggoseget is fel kell kesziteni a helyreallasra, mielott a forras
    // visszater RUNNING allapotba.
    if ((resource->errorInfo.faultSource == resource) &&
        resource->flags.recoveryPending &&
        !resource->flags.recoveryActive)
    {
        Resource_requestRecoveryForDependents(resource, resource);
    }
}
//------------------------------------------------------------------------------
// A hibaforras recoveryje eseten a PROPAGATE dependensek is kapjanak
// recovery-szandekot. A fuggosegi sorrend miatt ok csak a forras helyreallasa
// utan tudnak tenylegesen elindulni.
static void Resource_requestRecoveryForDependents(Resource_t* resource,
                                                   Resource_t* faultSource)
{
    ResourceDependency_t* dep;

    for (dep = resource->dependents; dep != NULL; dep = dep->nextDependent)
    {
        Resource_t* dependent = dep->dependent;

        if ((dep->errorStrategy != RESOURCE_DEP_PROPAGATE) ||
            !dep->internalUser.requested || (dependent->refCount == 0))
        {
            continue;
        }

        // A sajat aktiv DISABLED hibaval rendelkezo eroforras nem indulhat ujra
        // pusztan azert,
        // mert egy fuggosege helyreall. A blokkolt agon az alatta levo
        // eroforrasok sem
        // utemezhetok ujra.
        if (dependent->flags.faultProcessActive &&
            (dependent->errorInfo.faultSource == dependent) &&
            (dependent->recoveryOverride ==
             RESOURCE_RECOVERY_OVERRIDE_DISABLED))
        {
            Resource_cancelRecoveryForDependents(dependent, faultSource);
            continue;
        }

        // A fuggetlen recovery-kerest meg kell orizni. A keres csak akkor kapja
        // meg ennek
        // a kaszkadnak a jeloleset, ha meg nem vart sorban, vagy mar ehhez a
        // gyokerhibahoz
        // tartozik.
        if (!dependent->flags.recoveryPending ||
            (dependent->recoveryFaultSource == faultSource))
        {
            dependent->recoveryFaultSource = faultSource;
        }
        dependent->flags.recoveryPending = true;
        dependent->recoveryReason = RESOURCE_RECOVERY_REASON_DEPENDENCY;
        dependent->flags.recoveryFailed = false;
        // A recovery felulbiralas egy hibafolyamathoz tartozik.
        // Egy felso szintu hiba nem irhatja felul egy masik hibafolyamat
        // recovery beallitasat.
        if (resource->recoveryOverride > dependent->recoveryOverride)
        {
            if (dependent->flags.faultProcessActive &&
                (dependent->errorInfo.faultSource == faultSource))
            {
                dependent->recoveryOverride = resource->recoveryOverride;

                dependent->errorInfo.recoveryOverride =
                                                    resource->recoveryOverride;

                dependent->lastErrorInfo.recoveryOverride =
                                                    resource->recoveryOverride;
            }
            else if (!dependent->flags.faultProcessActive &&
                     dep->pendingError &&
                     (dep->pendingErrorInfo.faultSource == faultSource) &&
                     (resource->recoveryOverride >
                      dep->pendingErrorInfo.recoveryOverride))
            {
                // Ha a tovabbitott hibat meg nem dolgoztak fel, frissitsuk a
                // rogzitett masolatot. A Resource_process() ebbol a rekordbol
                // inicializalja majd a fuggoseg hibafolyamatat.
                dep->pendingErrorInfo.recoveryOverride =
                                                    resource->recoveryOverride;
            }
        }
        ResourceManager_queueResource(dependent);
        Resource_requestRecoveryForDependents(dependent, faultSource);
    }
}
//------------------------------------------------------------------------------
static void Resource_cancelRecoveryForDependents(
    Resource_t* resource, Resource_t* faultSource)
{
    ResourceDependency_t* dep;

    for (dep = resource->dependents; dep != NULL; dep = dep->nextDependent)
    {
        Resource_t* dependent = dep->dependent;
        bool activeFaultMatches;
        bool pendingFaultMatches;
        bool cascadeRequestMatches;
        bool independentRecoveryRequest;

        if (dep->errorStrategy != RESOURCE_DEP_PROPAGATE)
        {
            continue;
        }

        activeFaultMatches = dependent->flags.faultProcessActive &&
            (((dependent->errorInfo.status != kStatus_Success) &&
              (dependent->errorInfo.faultSource == faultSource)) ||
             ((dependent->errorInfo.status == kStatus_Success) &&
              (dependent->lastErrorInfo.faultSource == faultSource)));

        pendingFaultMatches = dep->pendingError &&
                            (dep->pendingErrorInfo.faultSource == faultSource);

        cascadeRequestMatches = dependent->flags.recoveryPending &&
                                (dependent->recoveryFaultSource == faultSource);

        // A resource szintu AUTO szabaly is onallo recovery-szandek, ha az
        // eroforras sajat
        // hibaja aktiv. Ehhez nem tartozik ResourceUser_t keres, ezert a user
        // recoveryRequested jelzoinel tobbet kell vizsgalni, kulonben egy
        // fuggetlen DISABLED kaszkad torolhetne.
        independentRecoveryRequest = !cascadeRequestMatches &&
            (Resource_hasRecoveryRequest(dependent) ||
             ((dependent->recoveryPolicy == RESOURCE_RECOVERY_POLICY_AUTO) &&
              (dependent->errorInfo.status != kStatus_Success) &&
              (dependent->errorInfo.faultSource == dependent) &&
              (dependent->recoveryOverride !=
               RESOURCE_RECOVERY_OVERRIDE_DISABLED)));

        // A DISABLED felulbiralas meg azelott megerkezhet, hogy a fuggoseg
        // feldolgozta volna a rogzitett hibat.
        // Torolni kell a varakozo kereset es a lancban tovabbadott recovery
        // szandekot is.
        if (pendingFaultMatches)
        {
            dep->pendingErrorInfo.recoveryOverride =
                                        RESOURCE_RECOVERY_OVERRIDE_DISABLED;
        }

        // A rogzitett felso szintu hiba nem bizonyitja, hogy a fuggoseg
        // recovery-kerese ehhez a kaszkadhoz tartozik. Az explicit
        // Resource_recover() keresnek tulelnie kell a fuggetlen tovabbitott
        // hiba megszakitasat. Csak a kaszkad gyokerforrasaval megjelolt
        // user keresek torolhetok.
        if ((activeFaultMatches || pendingFaultMatches ||
             cascadeRequestMatches) &&
            !dependent->flags.recoveryActive && !independentRecoveryRequest)
        {
            dependent->flags.recoveryPending = false;
            dependent->flags.recoveryCascadePending = false;
            dependent->flags.recoveryFailed = false;
            if (cascadeRequestMatches)
            {
                dependent->recoveryFaultSource = NULL;
                Resource_clearRecoveryRequests(dependent);
            }
        }

        Resource_cancelRecoveryForDependents(dependent, faultSource);
    }
}
//------------------------------------------------------------------------------
// Igaz, ha a hibaforrastol felfele, PROPAGATE kapcsolatokkal erintett teljes
// aktiv dependencia-lanc mar leallt. ERROR/WAITING allapot csak akkor szamit
// lealltnak, ha lifecycle callback mar nem aktiv.
static bool Resource_faultDependentsStopped(Resource_t* resource)
{
    ResourceDependency_t* dep;

    for (dep = resource->dependents; dep != NULL; dep = dep->nextDependent)
    {
        Resource_t* dependent = dep->dependent;

        if ((dep->errorStrategy != RESOURCE_DEP_PROPAGATE) ||
            !dep->internalUser.requested || (dependent->refCount == 0))
        {
            continue;
        }

        if (dependent->flags.lifecycleActive ||
            (dependent->state == RESOURCE_STARTING) ||
            (dependent->state == RESOURCE_RUNNING) ||
            (dependent->state == RESOURCE_STOPPING_))
        {
            return false;
        }

        if (!Resource_faultDependentsStopped(dependent))
        {
            return false;
        }
    }

    return true;
}
//------------------------------------------------------------------------------
// Eroforras allapot beallitasa es az erintettek pendingre helyezese.
// A manager mutexet a hivo tartja.
static void Resource_setStateInternal(Resource_t* resource,
                                      ResourceState_t state)
{
    ResourceDependency_t* dep;
    bool recoveryWasActive;

    if (resource->state == state)
    {
        return;
    }

    recoveryWasActive = resource->flags.recoveryActive;

    // Minden uj inditas uj generaciot kap. A terminalis allapotok is novelik
    // a generaciot, hogy a korabbi eletciklus kesleltetett jelzesei ne
    // modosithassak a STOPPED vagy DONE eroforrast a kovetkezo inditas elott.
    // A DONE mar ervenyteleniti a generaciot, ezert a belso STOPPED atmenet
    // nem igenyel ujabb ervenytelenitest.
    if ((state == RESOURCE_STARTING) ||
        (state == RESOURCE_DONE_) ||
        ((state == RESOURCE_STOPPED) &&
         (resource->state != RESOURCE_DONE_)))
    {
        resource->lifecycleGeneration++;
        if (resource->lifecycleGeneration == 0U)
        {
            resource->lifecycleGeneration = 1U;
        }
    }
    resource->state = state;

    // A kezdeti, eleve STOPPED allapot nem indithat leallasi callbacket.
    // Csak akkor jelezzuk majd az osszes-leallt allapotot, ha elotte a
    // rendszerben legalabb egy resource nem-STOPPED allapotba lepett.
    if ((state != RESOURCE_STOPPED) && (resource->manager != NULL))
    {
        resource->manager->allStoppedCallbackArmed = true;
    }

    if (state == RESOURCE_ERROR_)
    {
        if (resource->errorInfo.status == kStatus_Success)
        {
            Resource_initErrorInfo(&resource->errorInfo,
                                   kStatus_Fail,
                                   resource,
                                   resource,
                                   NULL,
                                   RESOURCE_ERROR_REASON_SELF_FAULT,
                                   resource->recoveryOverride);
            resource->lastErrorInfo = resource->errorInfo;
            resource->lastErrorStatus = kStatus_Fail;
            resource->flags.faultProcessActive = true;
        }

        // Az ERROR kezelese azonos legyen normal allapotvaltasnal es akkor is,
        // ha a hibajelzes mar aszinkron leallas kozben erkezik.
        Resource_applyErrorSideEffects(resource, recoveryWasActive);
    }
    else
    if (state == RESOURCE_STARTING)
    {
        // A callback azonnal lekerdezheti, hogy normal vagy recovery indulas.
        resource->startReason = resource->flags.recoveryActive ?
            RESOURCE_START_REASON_RECOVERY : RESOURCE_START_REASON_NORMAL;
        if (!resource->flags.recoveryActive)
        {
            resource->recoveryReason = RESOURCE_RECOVERY_REASON_NONE;
        }
        Resource_clearCurrentErrorInfo(resource);
        resource->flags.errorEventSent = false;
        resource->flags.runningEventSent = false;
        resource->flags.doneEventSent = false;
    }
    else
    if (state == RESOURCE_DONE_)
    {
        Resource_clearCurrentErrorInfo(resource);
        resource->flags.faultProcessActive = false;
        resource->recoveryOverride = RESOURCE_RECOVERY_OVERRIDE_DEFAULT;
        resource->flags.recoveryFailed = false;
        resource->flags.lifecycleActive = false;
        resource->flags.recoveryActive = false;
        resource->flags.recoveryPending = false;
        resource->flags.recoveryCascadePending = false;
        resource->recoveryAttempts = 0;
        resource->flags.errorEventSent = false;
        resource->flags.runningEventSent = false;
        resource->flags.doneEventSent = false;
    }
    else
    if (state == RESOURCE_STOPPED)
    {
        if (resource->refCount == 0)
        {
            Resource_clearCurrentErrorInfo(resource);
            resource->flags.faultProcessActive = false;
            resource->recoveryOverride = RESOURCE_RECOVERY_OVERRIDE_DEFAULT;
        }

        // A mar jelentett hibat a STOPPED atmenet nem torolheti es nem
        // jelentheti ujra. Csak hibamentes lifecycle eseten engedelyezzuk
        // ujra az ERROR esemenyt; az errorInfo-t az uj STARTING torli.
        if ((resource->errorInfo.status == kStatus_Success) ||
            (resource->errorInfo.resource == NULL))
        {
            resource->flags.errorEventSent = false;
        }
        resource->flags.runningEventSent = false;
        resource->flags.stoppedEventSent = false;
        resource->flags.lifecycleActive = false;
        resource->flags.stopRequestFailed = false;
        // A recovery probalkozas kozben a STOPPED csak a leallasi ciklus
        // resze. A recovery szamlalot csak sikeres RUNNING allapotban
        // szabad nullazni, kulonben minden ujraprobalkozasnal elveszne a
        // limit.
    }
    else
    if (state == RESOURCE_RUNNING)
    {
        // A recovery probalkozas szamlalojat itt nem szabad nullazni.
        // A Resource_process_running() ellenorzi a recoveryActive allapotot,
        // es ott kuldi ki a RECOVERED esemenyt. A szamlalo nullazasa is
        // csak a sikeres recovery lezartakor tortenhet meg.
        resource->flags.errorEventSent = false;
    }

    ResourceManager_queueResource(resource);

    // A dependency allapotvaltozasa csak a sajat dependenseit erinti.
    for (dep = resource->dependents; dep != NULL; dep = dep->nextDependent)
    {
        ResourceManager_queueResource(dep->dependent);
    }

    // A dependency allapotvaltozasa feloldhatja a teljes propagalt
    // dependent lanc leallasara varo recovery-t.
    for (dep = resource->dependencies; dep != NULL; dep = dep->next)
    {
        ResourceManager_queueResource(dep->resource);
    }
}
//------------------------------------------------------------------------------
// Manager altal eloallitott hiba beallitasa.
static void Resource_setInternalError(Resource_t* resource, status_t status)
{
    ResourceErrorInfo_t errorInfo;

    Resource_initErrorInfo(&errorInfo,
                           status,
                           resource,
                           resource,
                           NULL,
                           RESOURCE_ERROR_REASON_SELF_FAULT,
                           RESOURCE_RECOVERY_OVERRIDE_DEFAULT);
    Resource_copyErrorInfo(resource, &errorInfo);
    Resource_setStateInternal(resource, RESOURCE_ERROR_);
}
//------------------------------------------------------------------------------
// Resource inicializalasa.
// Inicializal egy resource objektumot a callbackekkel es a recovery-korlattal.
bool Resource_init(Resource_t* resource, const ResourceConfig_t* config)
{
    ResourceLoopExtension_t* loopExtension;

    // Inicializalja a resource belso allapotat es az alapertelmezett mezoket.
    // A fuggosegeket es a managert kesobb kell hozzarendelni.

    assert(resource);
    assert(config);
    assert(config->name);

    loopExtension = config->loopExtension;

    if ((loopExtension == NULL) &&
        ((config->funcs.startFunc != NULL) ||
         (config->funcs.stopFunc != NULL) ||
         (config->funcs.loopFunc != NULL) ||
         (config->funcs.errorFunc != NULL) ||
         (config->funcs.errorLoopStartFunc != NULL) ||
         (config->funcs.errorLoopFunc != NULL)))
    {
        return false;
    }

    memset(resource, 0, sizeof(*resource));

    resource->name = config->name;
    resource->state = RESOURCE_STOPPED;
    Resource_clearCurrentErrorInfo(resource);
    memset(&resource->lastErrorInfo, 0, sizeof(resource->lastErrorInfo));
    resource->lastErrorInfo.status = kStatus_Success;
    resource->lastErrorStatus = kStatus_Success;
    resource->flags.faultProcessActive = false;
    resource->recoveryPolicy = RESOURCE_RECOVERY_POLICY_STOP;
    resource->recoveryOverride = RESOURCE_RECOVERY_OVERRIDE_DEFAULT;
    resource->startReason = RESOURCE_START_REASON_NORMAL;
    resource->recoveryReason = RESOURCE_RECOVERY_REASON_NONE;
    resource->flags.recoveryFailed = false;
    resource->refCount = 0;
    resource->users = NULL;
    resource->usersLast = NULL;
    resource->observers = NULL;
    resource->observersLast = NULL;
    resource->dependencies = NULL;
    resource->dependents = NULL;
    resource->startRequestFunc = config->funcs.startRequestFunc;
    resource->stopRequestFunc = config->funcs.stopRequestFunc;
    resource->eventFunc = config->funcs.eventFunc;
    resource->loopExtension = loopExtension;
    resource->inputEventsAsync = 0U;

    if (loopExtension != NULL)
    {
        memset(loopExtension, 0, sizeof(*loopExtension));
        loopExtension->loopState = RESOURCE_LOOP_WAITING_FOR_START;
        loopExtension->loopControl.waitTime = portMAX_DELAY;
        loopExtension->loopStateInitPending = true;
        loopExtension->loopErrorCode = kStatus_Success;
        loopExtension->startFunc = config->funcs.startFunc;
        loopExtension->stopFunc = config->funcs.stopFunc;
        loopExtension->loopFunc = config->funcs.loopFunc;
        loopExtension->errorFunc = config->funcs.errorFunc;
        loopExtension->errorLoopStartFunc = config->funcs.errorLoopStartFunc;
        loopExtension->errorLoopFunc = config->funcs.errorLoopFunc;
    }
    resource->callbackData = config->callbackData;
    resource->manager = NULL;
    resource->flags.pending = false;
    resource->flags.restartAfterStop = false;
    resource->flags.stoppedDueToDependencyError = false;
    resource->flags.recoveryActive = false;
    resource->flags.recoveryPending = false;
    resource->recoveryFaultSource = NULL;
    resource->flags.recoveryCascadePending = false;
    resource->recoveryAttempts = 0;
    resource->recoveryMaxAttempts = config->recoveryMaxAttempts;
    resource->flags.lifecycleActive = false;
    resource->lifecycleGeneration = 0U;
    resource->flags.hasStarted = false;
    resource->flags.errorEventSent = false;
    resource->flags.runningEventSent = false;
    resource->flags.stoppedEventSent = true;
    return true;
}
//------------------------------------------------------------------------------
// Manager inicializalasa.
// Inicialitja a managert, a mutexet es az ebreszto szemaforat.
bool ResourceManager_init(ResourceManager_t* manager)
{
    // Letrehozza a manager mutexet, ebreszto szemaforat es kezdo listait.
    // Sikeres visszateres utan a manager fogadhat resource-okat.

    assert(manager);

    // Manager valtozoinak nullazasa
    memset(manager, 0, sizeof(*manager));

    // Mutex letrehozasa
    // Recursive mutex kell ahhoz, hogy callbackbol ujra hivhassuk
    // a Resource API-t.
    manager->mutex = xSemaphoreCreateRecursiveMutexStatic(
        &manager->mutexBuffer);
    if (manager->mutex == NULL)
    {
        return false;
    }

    manager->wakeup = xSemaphoreCreateBinaryStatic(&manager->wakeupBuffer);
    if (manager->wakeup == NULL)
    {
        vSemaphoreDelete(manager->mutex);
        manager->mutex = NULL;
        return false;
    }

    manager->resources = NULL;
    manager->lastResource = NULL;
    manager->pendingResources = NULL;
    manager->running = false;
    manager->initialized = true;
    manager->allStoppedCallback = NULL;
    manager->allStoppedCallbackData = NULL;
    manager->allStoppedCallbackArmed = false;

    return true;
}
//------------------------------------------------------------------------------
// Egyszeri manager-szintu callback regisztralasa.
// Csak a ResourceManager_run() elinditasa elott hivhato. A callback akkor fut,
// ha minden resource RESOURCE_STOPPED allapotu, minden refCount nulla, a
// lifecycle callbackek nem aktivak es a pending lista ures. ERROR, DONE vagy
// WAITING_FOR_DEPENDENCY allapot eseten nem hivodik meg. Nem hivodik meg
// indulaskor pusztan azert, mert kezdetben minden resource STOPPED.
// A callback a manager mutexen kivul fut.
bool ResourceManager_registerAllStoppedCallback(ResourceManager_t* manager,
                                ResourceManager_all_stopped_cb_t callback,
                                void* callbackData)
{
    assert(manager);

    if (!manager->initialized || (callback == NULL))
    {
        return false;
    }

    ResourceManager_lock(manager);

    // Egy callback regisztralhato, es csak a manager szal inditasa elott.
    if (manager->running || (manager->allStoppedCallback != NULL))
    {
        ResourceManager_unlock(manager);
        return false;
    }

    manager->allStoppedCallback = callback;
    manager->allStoppedCallbackData = callbackData;

    ResourceManager_unlock(manager);
    return true;
}
//------------------------------------------------------------------------------
// Resource hozzadasa a managerhez.
// (Csak init alatt hivhato.)
// A resource-t a managerhez rendeli. Csak init fazisban hivhato.
bool ResourceManager_addResource(ResourceManager_t* manager,
                                 Resource_t* resource)
{
    // A resource-t a managerhez kapcsolja, es felveszi a nyilvantartasba.
    // A muvelet ellenorzi a duplikaciot es a kezdeti allapot felteteleit.

    Resource_t* current;

    assert(manager);
    assert(resource);

    // Ha a manager nincs initelve, akkor nem lehet hozzaadni eroforrarst
    if (!manager->initialized)
    {
        return false;
    }

    ResourceManager_lock(manager);

    // Runtime kozben resource mar nem adhato hozza.
    if (manager->running)
    {
        ResourceManager_unlock(manager);
        return false;
    }

    // Resource nem tartozhat masik managerhez.
    if (resource->manager != NULL)
    {
        ResourceManager_unlock(manager);
        return false;
    }

    // Dupla regisztracio vedelme.
    for (current = manager->resources; current != NULL; current = current->next)
    {
        if (current == resource)
        {
            ResourceManager_unlock(manager);
            return false;
        }
    }

    // Megjegyzi a sajat manageret
    resource->manager = manager;

    // Az uj resource a lista vegere kerul, igy a bejaras sorrendje
    // megegyezik a ResourceManager_addResource() hivasok sorrendjevel.
    resource->next = NULL;
    if (manager->lastResource == NULL)
    {
        // Az elso resource egyszerre a lista eleje es vege.
        manager->resources = resource;
    }
    else
    {
        // Az eddigi utolso elem utan kapcsoljuk az uj resource-t.
        manager->lastResource->next = resource;
    }
    manager->lastResource = resource;

    ResourceManager_unlock(manager);
    return true;
}
//------------------------------------------------------------------------------
// Eroforras leirojanak lekerdezese az eroforras neve alapjan.
// NULL-t ad vissza, ha az eroforras nem talalhato.
Resource_t* ResourceManager_getResourceByName(ResourceManager_t* manager,
                                              const char*  name)
{
    Resource_t* resource=manager->resources;
    while(resource)
    {
        if (resource->name)
        {
            // Van beallitva hozza nev
            if (strcmp(resource->name, name) == 0)
            {
                // Egyezes. Visszaterunk az eroforrassal.
                return resource;
            }
        }
        // Lancolt lista kovetkezo elemere ugrunk
        resource = resource->next;
    }

    // Az eroforras nem talalhato
    return NULL;
}
//------------------------------------------------------------------------------
// User inicializalasa es hozzadasa.
// User kapcsolatot hoz letre egy resource igenylesehez.
// (Csak init alatt hivhato.)
bool ResourceUser_init(ResourceUser_t* user, const ResourceUserConfig_t* config)
{
    // Letrehoz egy logikai user kapcsolatot a resource igenylesehez.
    // A user sajat igenylesi es recovery jelzeseit kulon tarolja.

    assert(user);
    assert(config);
    assert(config->name);
    assert(config->resource);

    ResourceManager_t* manager;
    Resource_t* resource = config->resource;

    manager = resource->manager;
    assert(manager);

    ResourceManager_lock(manager);

    // User csak init kozben hozhato letre!
    if (manager->running)
    {
        ResourceManager_unlock(manager);
        return false;
    }

#if defined(RESOURCE_MANAGER_ENABLE_REGISTRATION_DUPLICATE_CHECK) && \
    (RESOURCE_MANAGER_ENABLE_REGISTRATION_DUPLICATE_CHECK != 0)
    // Opcionalis vedelem: kiszuri a korabban regisztralt user objektumot.
    if (ResourceUser_inUse(manager, user))
    {
        ResourceManager_unlock(manager);
        return false;
    }
#endif

    memset(user, 0, sizeof(*user));

    user->name = config->name;
    user->resource = resource;
    user->eventFunc = config->eventFunc;
    user->callbackData = config->callbackData;
    user->autoRecovery = false;

    // A tarolt lista-vegpointerrel a hozzaadas allando ideju.
    user->next = NULL;
    if (resource->usersLast == NULL)
    {
        resource->users = user;
    }
    else
    {
        resource->usersLast->next = user;
    }
    resource->usersLast = user;

    ResourceManager_unlock(manager);

    return true;
}
//------------------------------------------------------------------------------
// Resource esemenyfigyelo regisztralasa.
// KIZAROLAG az inicializalasi fazisban, a ResourceManager_run() inditasa
// elott hivhato! Nem hasznalja a manager mutexet!
bool ResourceObserver_init(ResourceObserver_t* observer,
                           const ResourceObserverConfig_t* config)
{
    // Letrehoz egy megfigyelot, amely esemenyekrol kap ertesitest.
    // A megfigyelo nem novelheti a resource igenylesi szamlalojat.

    ResourceManager_t* manager;

    assert(observer);
    assert(config);
    assert(config->name);
    assert(config->resource);
    assert(config->eventFunc);
    Resource_t* resource = config->resource;

    manager = resource->manager;
    assert(manager);

    // Szerzodes: ez a fuggveny kizarolag az inicializalasi fazisban,
    // a ResourceManager_run() elinditasa elott hivhato. Ebben a fazisban
    // nincs parhuzamos manager-mukodes vagy mas lista-modositas, ezert itt
    // szandekosan nem vesszuk fel a manager mutexet.
    // A running ellenorzes csak a hibas, kesoi hivasokat utasitja el; nem
    // teszi megengedette a regisztracio es a ResourceManager_run() versenyet.
    if (!manager->initialized || manager->running)
    {
        return false;
    }

#if defined(RESOURCE_MANAGER_ENABLE_REGISTRATION_DUPLICATE_CHECK) && \
    (RESOURCE_MANAGER_ENABLE_REGISTRATION_DUPLICATE_CHECK != 0)
    // Opcionalis vedelem: kiszuri a mar regisztralt observer objektumot.
    if (ResourceObserver_inUse(manager, observer))
    {
        return false;
    }
#endif

    memset(observer, 0, sizeof(*observer));
    observer->name = config->name;
    observer->resource = resource;
    observer->eventFunc =  config->eventFunc;
    observer->callbackData =  config->callbackData;

    // A lista vegere tesszuk a regisztracios sorrend megorzesehez.
    // A vegpointer miatt nincs szukseg a lista bejarasara.
    observer->next = NULL;
    if (resource->observersLast == NULL)
    {
        resource->observers = observer;
    }
    else
    {
        resource->observersLast->next = observer;
    }
    resource->observersLast = observer;

    return true;
}
//------------------------------------------------------------------------------
// Runtime enum-validalas
static bool Resource_isValidDependencyErrorStrategy(
                                ResourceDependencyErrorStrategy_t errorStrategy)
{
    switch (errorStrategy)
    {
        case RESOURCE_DEP_IGNORE:
        case RESOURCE_DEP_PROPAGATE:
        case RESOURCE_DEP_AUTO_RECOVER:
            return true;
        default:
            return false;
    }
}
//------------------------------------------------------------------------------
static bool Resource_isValidRecoveryPolicy(ResourceRecoveryPolicy_t policy)
{
    switch (policy)
    {
        case RESOURCE_RECOVERY_POLICY_STOP:
        case RESOURCE_RECOVERY_POLICY_AUTO:
            return true;
        default:
            return false;
    }
}
//------------------------------------------------------------------------------
static bool Resource_isValidState(ResourceState_t state)
{
    switch (state)
    {
        case RESOURCE_STOPPED:
        case RESOURCE_STARTING:
        case RESOURCE_RUNNING:
        case RESOURCE_STOPPING_:
        case RESOURCE_ERROR_:
        case RESOURCE_WAITING_FOR_DEPENDENCY:
        case RESOURCE_DONE_:
            return true;
        default:
            return false;
    }
}
//------------------------------------------------------------------------------
// Dependency hozzadasa.
// A dependency objektumot a hivo biztositja.
// A muvelet csak init alatt hivhato!
// A ciklusellenorzes alapertelmezesben ki van kapcsolva a sebesseg miatt.
// A konfiguraciot ciklusmentesen kell letrehozni. Ellenorzes bekapcsolasa:
// -DRESOURCE_MANAGER_ENABLE_DEPENDENCY_CYCLE_CHECK=1
bool Resource_addDependency(ResourceDependency_t* dependency,
                            Resource_t* resource,
                            Resource_t* dependencyResource,
                            ResourceDependencyErrorStrategy_t errorStrategy)
{
    // Itt jon letre a resource es a szukseges masik resource kapcsolata.
    // A ciklusvizsgalat megakadalyozza a koros fuggosegi lancot.

    assert(resource);
    assert(dependency);
    assert(dependencyResource);

    if (!Resource_isValidDependencyErrorStrategy(errorStrategy))
    {
        return false;
    }

    if ((resource == NULL) || (dependency == NULL) ||
        (dependencyResource == NULL))
    {
        return false;
    }

    if (resource == dependencyResource)
    {
        return false;
    }

    ResourceManager_t* manager = resource->manager;
    assert(manager);

    if (dependencyResource->manager != manager)
    {
        return false;
    }

    ResourceManager_lock(manager);

    if (manager->running)
    {
        ResourceManager_unlock(manager);

        return false;
    }

#if defined(RESOURCE_MANAGER_ENABLE_REGISTRATION_DUPLICATE_CHECK) && \
    (RESOURCE_MANAGER_ENABLE_REGISTRATION_DUPLICATE_CHECK != 0)
    // Opcionalis vedelem: kiszuri a mar hasznalt dependency objektumot.
    if (Resource_dependencyInUse(manager, dependency))
    {
        ResourceManager_unlock(manager);
        return false;
    }
#endif

    // Ugyanaz a dependency resource csak egyszer lehet ugyanennek
    // a resource-nak a fuggosege.
    ResourceDependency_t* curr;
    for (curr = resource->dependencies; curr != NULL; curr = curr->next)
    {
        if (curr->resource == dependencyResource)
        {
            ResourceManager_unlock(manager);
            return false;
        }
    }

#if defined(RESOURCE_MANAGER_ENABLE_DEPENDENCY_CYCLE_CHECK) && \
    (RESOURCE_MANAGER_ENABLE_DEPENDENCY_CYCLE_CHECK != 0)
    // Ha a cel mar elerheto a dependencyResource felol, a kapcsolat ciklust
    // hozna letre. Ez az ellenorzes csak a forditasi kapcsoloval aktiv.
    if (Resource_graphContains(dependencyResource, resource))
    {
        ResourceManager_unlock(manager);
        return false;
    }
#endif

    memset(dependency, 0, sizeof(*dependency));

    dependency->resource = dependencyResource;
    dependency->dependent = resource;
    dependency->errorStrategy = errorStrategy;

    // A belso user nem kuld callbacket.
    // A resource dependency objektum sajat userkent igenyli a dependency
    // resource-t.
    dependency->internalUser.name = resource->name;
    dependency->internalUser.resource = dependencyResource;
    dependency->internalUser.requested = false;
    dependency->internalUser.recoveryRequested = false;
    dependency->internalUser.eventFunc = NULL;
    dependency->internalUser.callbackData = dependency;

    // Dependency lista elejere tesszuk.
    dependency->next = resource->dependencies;
    resource->dependencies = dependency;

    // A belso user is a userlista vegere kerul, igy a refcount es a recovery
    // kezeles egyseges marad. A vegpointer miatt nincs lista-bejaras.
    dependency->internalUser.next = NULL;
    if (dependencyResource->usersLast == NULL)
    {
        dependencyResource->users = &dependency->internalUser;
    }
    else
    {
        dependencyResource->usersLast->next = &dependency->internalUser;
    }
    dependencyResource->usersLast = &dependency->internalUser;

    // A dependency resource dependenseinek listajaba is bekerul.
    dependency->nextDependent = dependencyResource->dependents;
    dependencyResource->dependents = dependency;

    ResourceManager_unlock(manager);

    return true;
}
//------------------------------------------------------------------------------
// Userenkenti automatikus recovery beallitasa.
// Alapertelmezetten letiltva.
// Engedelyezeskor egy mar fennallo, sajat vagy propagalt dependency-hiba is
// recoveryt indithat. A beallitas futas kozben is modosithato.
// A mar kiadott recovery kerest a letiltas nem vonja vissza.
void ResourceUser_setAutoRecovery(ResourceUser_t* user, bool enabled)
{
    // A recovery automatikus inditasat az adott userhez koti.
    // A beallitas nem irja felul a resource recovery policy-jat.

    assert(user);
    assert(user->resource);

    Resource_t* resource = user->resource;
    ResourceManager_t* manager = resource->manager;
    assert(manager);

    ResourceManager_lock(manager);
    user->autoRecovery = enabled;

    if (enabled && ResourceUser_requestAutoRecoveryOnError(user))
    {
        ResourceManager_queueResource(resource);
    }
    ResourceManager_unlock(manager);
}
//------------------------------------------------------------------------------
// Resource igenylese.
// Nem inditja el azonnal a resource-t, Az a manager szalban teljesul.
void Resource_request(ResourceUser_t* user)
{
    // A user igenyli a resource mukodeset.
    // Az igenyles idempotens; valtozas eseten a manager ujra feldolgoz.

    assert(user);
    Resource_t* resource = user->resource;
    assert(resource);

    ResourceManager_t* manager = resource->manager;
    assert(manager);

    // Ha meg nincs az adott userben hasznalati kerelem, akkor az adott
    // eroforras referencia szamlalojat noveli.
    ResourceManager_lock(manager);
    if (!user->requested)
    {
        user->requested = true;

        // Referencias szamlalo novelese
        resource->refCount++;

        // Ha a user egy mar hibas eroforrast kezd igenyelni, az autoRecovery
        // beallitasa akkor is ervenyesul, ha a hiba egy dependency-bol
        // propagalodott. A queue-olas lentebb megtortenik.
        (void)ResourceUser_requestAutoRecoveryOnError(user);

        // Ha a resource mar teljesen jelezte a RUNNING allapotot, az uj
        // igenylo kulon STARTED vissza jelzest kap. Ha a RUNNING esemeny
        // feldolgozasa meg hatra van, akkor a szokasos broadcast fogja
        // ertesiteni, igy nincs dupla callback.
        if ((resource->state == RESOURCE_RUNNING) &&
            resource->flags.runningEventSent &&
            (user->eventFunc != NULL))
        {
            user->startedEventPending = true;
        }

        // A DONE esemeny kikuldese utan is lehet masik usernek aktiv kerese.
        // Az uj user terminalis eredmenyet a manager szal jelzi, ideertve azt
        // az esetet is, amikor a keres egy korabbi DONE callbackbol erkezik.
        if ((resource->state == RESOURCE_DONE_) &&
            resource->flags.doneEventSent &&
            (user->eventFunc != NULL))
        {
            user->doneEventPending = true;
        }

        ResourceManager_queueResource(resource);
    }
    ResourceManager_unlock(manager);
}
//------------------------------------------------------------------------------
// Resource elengedese.
// Nem allitja le azonnal a resource-t. Az a manager szalban teljesul
void Resource_release(ResourceUser_t* user)
{
    // Megszunteti a user igenyleset, ha az aktiv volt.
    // A resource csak akkor allhat le, ha mar nincs aktiv igeny.

    assert(user);
    Resource_t* resource = user->resource;
    assert(resource);

    ResourceManager_t* manager = resource->manager;
    assert(manager);

    ResourceManager_lock(manager);

    // Ha hasznalatban van az eroforras az user altal, akkor csokkenti az
    // eroforras referencia-szamlalojat.
    if (user->requested)
    {
        user->requested = false;
        user->startedEventPending = false;
        user->doneEventPending = false;
        user->recoveryRequested = false;

        // Referencia-szamlalo csokkentese.
        assert(resource->refCount > 0);
        resource->refCount--;

        if ((resource->refCount == 0) &&
            (resource->state == RESOURCE_STOPPED))
        {
            // A mar leallitott resource regi hibaja ne szivarogjon at
            // egy kesobbi, uj igenylesbe.
            Resource_clearCurrentErrorInfo(resource);
            resource->flags.faultProcessActive = false;
            resource->recoveryOverride = RESOURCE_RECOVERY_OVERRIDE_DEFAULT;
        }
        ResourceManager_queueResource(resource);
    }

    ResourceManager_unlock(manager);
}
#if defined(RESOURCE_MANAGER_ENABLE_DEVELOPMENT_API) && \
    RESOURCE_MANAGER_ENABLE_DEVELOPMENT_API
//------------------------------------------------------------------------------
// Eroforras kozvetlen igenylese fejlesztesi es konzolos tesztelesi celra.
// Nem kell hozza user objektum; minden hivas egy referencia-t ad hozza.
// Nem idempotens, ezert minden hivast Resource_stop() hivasnak kell kovetnie.
// Eles alkalmazasi kodban ezt az API-t nem szabad hasznalni.
void Resource_start(Resource_t* resource)
{
    ResourceManager_t* manager;

    assert(resource);
    manager = resource->manager;
    assert(manager);

    ResourceManager_lock(manager);

    // Egy kozvetlen fejlesztoi igenyles tartja eletben az eroforrast.
    resource->refCount++;
    ResourceManager_queueResource(resource);

    ResourceManager_unlock(manager);
}
//------------------------------------------------------------------------------
// Eroforras kozvetlen igenylesenek elengedese fejlesztesi es konzolos celra.
// Nullas referencia-szamlalot nem csokkent, igy nem fordul elo alaferdules.
// Eles alkalmazasi kodban ezt az API-t nem szabad hasznalni.
void Resource_stop(Resource_t* resource)
{
    ResourceManager_t* manager;

    assert(resource);
    manager = resource->manager;
    assert(manager);

    ResourceManager_lock(manager);

    if (resource->refCount > 0U)
    {
        resource->refCount--;

        if ((resource->refCount == 0U) &&
            (resource->state == RESOURCE_STOPPED))
        {
            // A korabbi lifecycle hibaja ne szivarogjon at egy uj igenylesbe.
            Resource_clearCurrentErrorInfo(resource);
            resource->flags.faultProcessActive = false;
            resource->recoveryOverride = RESOURCE_RECOVERY_OVERRIDE_DEFAULT;
        }

        ResourceManager_queueResource(resource);
    }

    ResourceManager_unlock(manager);
}
#endif
//------------------------------------------------------------------------------
// Recovery igenylese.
// Nem inditja el azonnal a recoveryt. Az a manager szalban teljesul.
void Resource_recover(ResourceUser_t* user)
{
    // A user recovery kerest ad le a resource fele.
    // A tenyleges helyreallitast a manager allapotgepe vegzi.

    assert(user);
    Resource_t* resource = user->resource;
    assert(resource);

    ResourceManager_t* manager = resource->manager;
    assert(manager);

    ResourceManager_lock(manager);

    // Recovery ERROR/WAITING allapotban kerheto. Ha a hiba mar rogzitett,
    // RUNNING/STOPPING allapotban is el kell fogadni a callbackbol erkezo
    // kereset, mert a resource leallitasa meg csak ezutan kovetkezik.
    if (user->requested && !resource->flags.recoveryActive &&
        ((resource->state == RESOURCE_ERROR_) ||
         (resource->state == RESOURCE_WAITING_FOR_DEPENDENCY) ||
         (((resource->state == RESOURCE_RUNNING) ||
           (resource->state == RESOURCE_STOPPING_) ||
           (resource->state == RESOURCE_STOPPED)) &&
          (resource->errorInfo.status != kStatus_Success) &&
          (resource->errorInfo.resource != NULL))))    {
        // A limit az automatikus recovery ciklust korlatozza. Uj explicit
        // keres uj ciklust indithat, ezert a szamlalot ujrainditjuk.
        if ((resource->recoveryMaxAttempts != 0) &&
            (resource->recoveryAttempts >= resource->recoveryMaxAttempts))
        {
            Resource_clearRecoveryRequests(resource);
            resource->recoveryAttempts = 0;
            resource->flags.recoveryActive = false;
            resource->flags.recoveryPending = false;
            resource->flags.recoveryCascadePending = false;
        }

        user->recoveryRequested = true;
        resource->flags.recoveryPending = true;
        // Az explicit keres a userhez tartozik, nem a felso szintu automatikus
        // kaszkadhoz.
        // Fuggetlennek jeloljuk, hogy a kaszkad megszakitasa ne fogyaszthassa
        // el ezt a kereset.
        resource->recoveryFaultSource = NULL;
        resource->recoveryReason =
            RESOURCE_RECOVERY_REASON_HIGHER_LAYER_REQUEST;
        resource->flags.recoveryFailed = false;
        if (user->autoRecovery)
        {
            resource->flags.recoveryCascadePending = true;
        }
        ResourceManager_queueResource(resource);
    }

    ResourceManager_unlock(manager);
}
//------------------------------------------------------------------------------
// Aszinkron resource allapotjelzes belso implementacioja.
static void Resource_setStateImpl(Resource_t* resource,
                                  bool checkGeneration,
                                  uint32_t generation,
                                  ResourceState_t state,
                                  status_t status)
{
    // Kozponti allapotvaltas: itt frissul az allapot es az esemenyek.
    // Az atmenethez kotott mellekhatasok egy helyen maradnak.

    assert(resource);

    if ((resource == NULL) || !Resource_isValidState(state) ||
        ((state == RESOURCE_ERROR_) ?
         (status == kStatus_Success) :
         (status != kStatus_Success)))
    {
        return;
    }

    ResourceManager_t* manager = resource->manager;
    assert(manager);

    ResourceManager_lock(manager);

    if (checkGeneration && (generation != resource->lifecycleGeneration))
    {
        ResourceManager_unlock(manager);
        return;
    }

    // Egy hibas stopRequest utani STOPPED jelzes lezarthatja a lifecycle-t.
    bool stopFailureStopCompletion =
        (resource->state == RESOURCE_ERROR_) &&
        (state == RESOURCE_STOPPED) &&
        resource->flags.stopRequestFailed &&
        resource->flags.lifecycleActive;

    // Az ERROR tartos allapot: a resource sajat szala nem torolheti.
    // Kivetel a hibas stopRequest utani, aktiv lifecycle-hoz tartozo STOPPED.
    // Az errorInfo ilyenkor a lastErrorInfo mezoben megmarad.
    if ((resource->state == RESOURCE_ERROR_) &&
        !stopFailureStopCompletion)
    {
        ResourceManager_unlock(manager);
        return;
    }

    // STOPPED csak STOPPING alatt, vagy hibas stopRequest utani
    // visszaigazolasra fogadhato el. STARTING/RUNNING alatt egy regi
    // visszajelzes ervenyteleniti az uj lifecycle-t.
    if ((state == RESOURCE_STOPPED) &&
        (resource->state != RESOURCE_STOPPING_) &&
        (resource->state != RESOURCE_STOPPED) &&
        !stopFailureStopCompletion)
    {
        ResourceManager_unlock(manager);
        return;
    }

    // RUNNING es DONE csak aktiv inditasi/futasi ciklusbol fogadhato el.
    // Egy kesve beerkezo sikerjelzes nem irhatja felul a mar megkezdett vagy
    // befejezett leallitast, kulonben ujabb stop callbacket indithat el.
    if (((state == RESOURCE_RUNNING) &&
         (resource->state != RESOURCE_STARTING) &&
         (resource->state != RESOURCE_RUNNING)) ||
        ((state == RESOURCE_DONE_) &&
         (resource->state != RESOURCE_STARTING) &&
         (resource->state != RESOURCE_RUNNING) &&
         (resource->state != RESOURCE_DONE_)))
    {
        ResourceManager_unlock(manager);
        return;
    }

    // Hibas leallitas kozben csak a STOPPED jelzes fogadhato el. Egy keson
    // erkezo mas allapotjelzes nem torolheti a mar rogzitett hibat.
    if ((resource->state == RESOURCE_STOPPING_) &&
        (resource->errorInfo.status != kStatus_Success) &&
        (state != RESOURCE_STOPPED))
    {
        ResourceManager_unlock(manager);
        return;
    }

    if (state == RESOURCE_ERROR_)
    {
        ResourceErrorInfo_t errorInfo;

        // Recovery-attempt hiba eseten az elozo hibafolyamat eredeti adatai
        // maradnak ervenyben; a mostani hiba csak a recovery probalkozast zarja
        // le.
        if (resource->flags.faultProcessActive)
        {
            if (resource->flags.recoveryActive &&
                (resource->errorInfo.status == kStatus_Success))
            {
                resource->errorInfo = resource->lastErrorInfo;
            }
            else if ((resource->errorInfo.status != kStatus_Success) &&
                     (resource->errorInfo.resource != NULL))
            {
                ResourceManager_queueResource(resource);
                ResourceManager_unlock(manager);
                return;
            }
            else if (!resource->flags.recoveryActive)
            {
                ResourceManager_queueResource(resource);
                ResourceManager_unlock(manager);
                return;
            }
        }
        else
        {
            Resource_initErrorInfo(&errorInfo,
                                   status,
                                   resource,
                                   resource,
                                   NULL,
                                   RESOURCE_ERROR_REASON_SELF_FAULT,
                                   RESOURCE_RECOVERY_OVERRIDE_DEFAULT);
            Resource_copyErrorInfo(resource, &errorInfo);
        }
    }
    else if ((state == RESOURCE_RUNNING) || (state == RESOURCE_DONE_))
    {
        ResourceErrorInfo_t errorInfo;

        Resource_initErrorInfo(&errorInfo,
                               kStatus_Success,
                               NULL,
                               NULL,
                               NULL,
                               RESOURCE_ERROR_REASON_NONE,
                               RESOURCE_RECOVERY_OVERRIDE_DEFAULT);
        Resource_copyErrorInfo(resource, &errorInfo);
    }

    if ((state == RESOURCE_ERROR_) &&
        (resource->state == RESOURCE_STOPPING_))
    {
        // Helyi hiba aszinkron leallitasi callback kozben is jelentkezhet.
        // Tartsuk meg a STOPPING allapotot, es varjuk meg a callback STOPPED
        // visszajelzeset ahelyett, hogy masodszor is meghivnank a stopot.
        // A STOPPING feldolgozasa ertesiti a usereket az uj helyi hibarol.
        bool recoveryWasActive = resource->flags.recoveryActive;
        Resource_applyErrorSideEffects(resource, recoveryWasActive);
        ResourceManager_queueResource(resource);
        ResourceManager_unlock(manager);
        return;
    }

    Resource_setStateInternal(resource, state);
    ResourceManager_unlock(manager);
}
//------------------------------------------------------------------------------
// Lifecycle-generacio lekerdezese.
// A start/stop callbackben, meg az aszinkron munka elinditasa elott kell
// lekerdezni es a workernek atadni.
// A STOPPED/DONE terminalis atmenet ervenyteleniti a lezart lifecycle
// generaciojat is.
uint32_t Resource_getLifecycleGeneration(Resource_t* resource)
{
    uint32_t generation;
    ResourceManager_t* manager;

    assert(resource);
    manager = resource->manager;
    assert(manager);

    ResourceManager_lock(manager);
    generation = resource->lifecycleGeneration;
    ResourceManager_unlock(manager);

    return generation;
}
//------------------------------------------------------------------------------
// Generaciohoz kotott aszinkron allapotjelzes.
// A regi lifecycle-bol kesve beerkezo jelzeseket a manager eldobja.
// RESOURCE_ERROR eseten status nem lehet kStatus_Success.
// A tobbi allapotnal status legyen kStatus_Success.
void Resource_setStateForGeneration(Resource_t* resource,
                                    uint32_t generation,
                                    ResourceState_t state,
                                    status_t status)
{
    Resource_setStateImpl(resource, true, generation, state, status);
}
//------------------------------------------------------------------------------
// Legacy allapotjelzes. Szinkron callbackbol hasznalhato, vagy ha a hivo
// garantalja, hogy nincs regi lifecycle-bol kesve beerkezo statusz.
// Aszinkron workerbol a generacios API hasznalata javasolt.
void Resource_setState(Resource_t* resource,
                       ResourceState_t state,
                       status_t status)
{
    Resource_setStateImpl(resource, false, 0U, state, status);
}
//------------------------------------------------------------------------------
static bool Resource_reportErrorImpl(Resource_t* reporter,
                                     bool checkGeneration,
                                     uint32_t generation,
                                     Resource_t* faultSource,
                                     status_t status,
                                    ResourceRecoveryOverride_t recoveryOverride)
{
    // Feldolgozza a hibajelentest, es megorzi a hiba eredetet.
    // A generacio ellenorzese kiszuri a korabbi eletciklus kesoi hibait.

    ResourceManager_t* manager;
    ResourceErrorInfo_t errorInfo;
    bool recoveryWasActive;
    bool wasFaultProcessActive;

    if ((reporter == NULL) || (faultSource == NULL) ||
        (status == kStatus_Success) ||
        (recoveryOverride < RESOURCE_RECOVERY_OVERRIDE_DEFAULT) ||
        (recoveryOverride > RESOURCE_RECOVERY_OVERRIDE_FORCED) ||
        (reporter->manager == NULL) ||
        (reporter->manager != faultSource->manager) ||
        !Resource_graphContains(reporter, faultSource))
    {
        return false;
    }

    manager = reporter->manager;
    ResourceManager_lock(manager);

    if (checkGeneration && (generation != reporter->lifecycleGeneration))
    {
        ResourceManager_unlock(manager);
        return false;
    }

    recoveryWasActive = faultSource->flags.recoveryActive;
    wasFaultProcessActive = faultSource->flags.faultProcessActive;

    if (!faultSource->flags.faultProcessActive)
    {
        ResourceErrorReason_t reason = (reporter == faultSource) ?
            RESOURCE_ERROR_REASON_SELF_FAULT :
            RESOURCE_ERROR_REASON_REPORTED_FAULT;
        Resource_initErrorInfo(&errorInfo,
                               status,
                               faultSource,
                               reporter,
                               NULL,
                               reason,
                               recoveryOverride);
        Resource_copyErrorInfo(faultSource, &errorInfo);
    }
    else
    {
        // Az elso hiba adatai maradnak; ha a recovery-kiserlet kozben a
        // pillanatnyi errorInfo ki lett uritve, a diagnosztikai rekordbol
        // visszaallitjuk az eredeti hibafolyamatot.
        if (faultSource->errorInfo.status == kStatus_Success)
        {
            faultSource->errorInfo = faultSource->lastErrorInfo;
        }
        if (recoveryOverride > faultSource->recoveryOverride)
        {
            faultSource->recoveryOverride = recoveryOverride;
            faultSource->errorInfo.recoveryOverride = recoveryOverride;
            faultSource->lastErrorInfo.recoveryOverride = recoveryOverride;
        }
    }

    // A hibaforras recovery-strategiaja es az esemeny felulbiralasa dont.
    if (faultSource->recoveryOverride == RESOURCE_RECOVERY_OVERRIDE_FORCED)
    {
        if (!faultSource->flags.recoveryActive &&
            ((faultSource->recoveryMaxAttempts == 0) ||
             (faultSource->recoveryAttempts <
              faultSource->recoveryMaxAttempts)))
        {
            faultSource->flags.recoveryPending = true;
            faultSource->recoveryReason =
                Resource_reasonForError(&faultSource->errorInfo);
            faultSource->flags.recoveryFailed = false;
        }
        if (faultSource->flags.recoveryActive ||
            faultSource->flags.recoveryPending)
        {
            // Ha a FORCED mar folyamatban levo recovery kozben erkezik, akkor
            // is gondoskodjunk arrol, hogy a teljes tovabbitott fuggosegi lanc
            // kovesse a helyreallitast.
            Resource_requestRecoveryForDependents(faultSource, faultSource);
        }
    }
    else if ((faultSource->recoveryOverride ==
              RESOURCE_RECOVERY_OVERRIDE_DISABLED) &&
             !faultSource->flags.recoveryActive)
    {
        faultSource->flags.recoveryPending = false;
        faultSource->flags.recoveryCascadePending = false;
        faultSource->flags.recoveryFailed = false;
        Resource_clearRecoveryRequests(faultSource);
        Resource_cancelRecoveryForDependents(faultSource, faultSource);
    }

    if (faultSource->state == RESOURCE_STOPPING_)
    {
        if (!wasFaultProcessActive)
        {
            Resource_applyErrorSideEffects(faultSource, recoveryWasActive);
        }
        if (faultSource->flags.stopRequestFailed)
        {
            Resource_setStateInternal(faultSource, RESOURCE_ERROR_);
        }
        else
        {
            ResourceManager_queueResource(faultSource);
        }
    }
    else if (faultSource->state != RESOURCE_ERROR_)
    {
        Resource_setStateInternal(faultSource, RESOURCE_ERROR_);
    }
    else
    {
        if (!wasFaultProcessActive)
        {
            Resource_applyErrorSideEffects(faultSource, recoveryWasActive);
        }
        ResourceManager_queueResource(faultSource);
    }

    ResourceManager_unlock(manager);
    return true;
}
//------------------------------------------------------------------------------
// Hibajelzes egy eroforrasra.
// A reporter a faultSource erofrast jeloli meg a hiba forrasakent.
// A faultSource-nak a reporter dependenciafajaban kell lennie.
// A hibaforras es a hibastatusz az elso elfogadott jelzesbol marad meg.
// A recoveryOverride viszont az aktiv hibafolyamat alatt a
// FORCED > DISABLED > DEFAULT sorrendben erosodhet.
// Igaz ertekkel ter vissza, ha elfogadta a hibajelzest.
bool Resource_reportError(Resource_t* reporter,
                          Resource_t* faultSource,
                          status_t status,
                          ResourceRecoveryOverride_t recoveryOverride)
{
    return Resource_reportErrorImpl(reporter, false, 0U, faultSource,
                                    status, recoveryOverride);
}
//------------------------------------------------------------------------------
// Csak a megadott eletciklushoz tartozo hibat fogadja el.
// (Resource_reportError-nal leirtak ervenyesek ra is)
bool Resource_reportErrorForGeneration(Resource_t* reporter,
                                       uint32_t generation,
                                       Resource_t* faultSource,
                                       status_t status,
                                ResourceRecoveryOverride_t recoveryOverride)
{
    return Resource_reportErrorImpl(reporter, true, generation, faultSource,
                                    status, recoveryOverride);
}
//------------------------------------------------------------------------------
// A hibaforras alapertelmezett recovery-strategiajanak beallitasa.
void Resource_setRecoveryPolicy(Resource_t* resource,
                                ResourceRecoveryPolicy_t policy)
{
    // Beallitja a resource alap recovery viselkedeset.
    // A hibahoz tartozo override ettol kulon, erossegi sorrendben ervenyesul.

    ResourceManager_t* manager;

    assert(resource);
    if ((resource == NULL) || !Resource_isValidRecoveryPolicy(policy))
    {
        return;
    }
    manager = resource->manager;
    assert(manager);
    if (manager == NULL)
    {
        return;
    }

    ResourceManager_lock(manager);
    resource->recoveryPolicy = policy;
    if ((policy == RESOURCE_RECOVERY_POLICY_AUTO) &&
        (resource->errorInfo.status != kStatus_Success) &&
        (resource->errorInfo.faultSource == resource) &&
        (resource->recoveryOverride == RESOURCE_RECOVERY_OVERRIDE_DEFAULT))
    {
        resource->flags.recoveryPending = true;
        resource->recoveryReason =
            Resource_reasonForError(&resource->errorInfo);
        resource->flags.recoveryFailed = false;
        if (resource->errorInfo.faultSource == resource)
        {
            Resource_requestRecoveryForDependents(resource, resource);
        }
        ResourceManager_queueResource(resource);
    }
    ResourceManager_unlock(manager);
}
//------------------------------------------------------------------------------
// Az eroforras inditasi okanak lekerdezese
// Callbackbol is hivhato informacio lekerdezes.
ResourceStartReason_t Resource_getStartReason(Resource_t* resource)
{
    ResourceStartReason_t result;
    ResourceManager_t* manager;
    assert(resource);
    manager = resource->manager;
    assert(manager);
    ResourceManager_lock(manager);
    result = resource->startReason;
    ResourceManager_unlock(manager);
    return result;
}
//------------------------------------------------------------------------------
// Az eroforras hiba okanak lekerdezese
// Callbackbol is hivhato informacio lekerdezes.
ResourceErrorReason_t Resource_getErrorReason(Resource_t* resource,
                                              ResourceErrorInfo_t* errorInfo)
{
    ResourceErrorReason_t result;
    ResourceManager_t* manager;
    assert(resource);
    manager = resource->manager;
    assert(manager);
    ResourceManager_lock(manager);
    result = resource->lastErrorInfo.reason;
    if (errorInfo != NULL)
    {
        *errorInfo = resource->lastErrorInfo;
    }
    ResourceManager_unlock(manager);
    return result;
}
//------------------------------------------------------------------------------
// Az eroforras helyreallitasi okanak lekerdezese
// Callbackbol is hivhato informacio lekerdezes.
ResourceRecoveryReason_t Resource_getRecoveryReason(Resource_t* resource)
{
    ResourceRecoveryReason_t result;
    ResourceManager_t* manager;
    assert(resource);
    manager = resource->manager;
    assert(manager);
    ResourceManager_lock(manager);
    result = resource->recoveryReason;
    ResourceManager_unlock(manager);
    return result;
}
//------------------------------------------------------------------------------
// A recovery sikeressegenek lekerdezese.
// Callbackbol is hivhato informacio lekerdezes.
// Igaz, ha a recovery-probalkozasi korlat sikertelenul elfogyott.
bool Resource_recoveryFailed(Resource_t* resource)
{
    bool result;
    ResourceManager_t* manager;
    assert(resource);
    manager = resource->manager;
    assert(manager);
    ResourceManager_lock(manager);
    result = resource->flags.recoveryFailed;
    ResourceManager_unlock(manager);
    return result;
}
//------------------------------------------------------------------------------
// STOPPED allapot feldolgozasa
static void Resource_process_stopped(Resource_t* resource)
{
    // STOPPED allapotban az igenyeket es a fuggosegeket vizsgaljuk.
    // Inditas csak akkor kezdodik, ha az osszes szukseges feltetel teljesul.

    ResourceManager_t* manager;
    bool errorFound;
    bool recoveryRequested;
    bool userRecoveryRequested;
    bool dependenciesReady;
    ResourceErrorInfo_t dependencyErrorInfo;
    manager = resource->manager;

    ResourceManager_lock(manager);

    // A STOPPED visszajelzessel versenyezhet egy, az utolso STOPPING ellenorzes
    // utan beerkezo fuggosegi hiba. A STOPPED esemeny elott vizsgaljuk meg a
    // rogzitett fuggosegi hibat, hogy az esemenyek sorrendje ERROR elso legyen.
    if (!resource->flags.stoppedEventSent && (resource->refCount > 0))
    {
        Resource_requestDependencies(resource);
        (void)Resource_checkDependencies(resource,
                                         &errorFound,
                                         &recoveryRequested,
                                         &dependencyErrorInfo);

        if ((errorFound || recoveryRequested) &&
            (dependencyErrorInfo.resource != NULL) &&
            (resource->errorInfo.status == kStatus_Success))
        {
            Resource_copyErrorInfo(resource, &dependencyErrorInfo);
        }

        // A keson beerkezo fuggosegi hiba azt jelenti, hogy a leallast a
        // fuggoseg okozta. Orizzuk meg ezt, hogy az AUTO_RECOVER helyreallitsa
        // a fuggoseget, de ne inditsa ujra csendben a mar futott fuggot.
        if ((errorFound || recoveryRequested) &&
            (resource->errorInfo.status != kStatus_Success) &&
            (resource->errorInfo.resource != NULL) &&
            (resource->errorInfo.resource != resource))
        {
            resource->flags.stoppedDueToDependencyError = true;
        }

        if (recoveryRequested)
        {
            Resource_requestDependencyRecovery(resource);
        }

        if ((errorFound || recoveryRequested) &&
            (resource->errorInfo.status != kStatus_Success) &&
            (resource->errorInfo.resource != NULL) &&
            !resource->flags.errorEventSent)
        {
            resource->flags.errorEventSent = true;
            Resource_notifyEvent(resource, RESOURCE_EVENT_ERROR,
                                 &resource->errorInfo);

            // Az ERROR callback recoveryt kerhet akkor is, ha az eroforras mar
            // STOPPED. A callback altal okozott allapotvaltas megszakitja ezt
            // a feldolgozasi kort.
            if (resource->state != RESOURCE_STOPPED)
            {
                ResourceManager_unlock(manager);
                return;
            }
        }
    }

    if (!resource->flags.stoppedEventSent)
    {
        resource->flags.stoppedEventSent = true;
        Resource_notifyEvent(resource, RESOURCE_EVENT_STOPPED, NULL);

        // Az esemeny callback reentrans lehet, allapotot is valtoztathat.
        // Ha ez megtortent, a regi STOPPED feldolgozas mar nem folytathato.
        if (resource->state != RESOURCE_STOPPED)
        {
            ResourceManager_unlock(manager);
            return;
        }
    }

    if (resource->refCount == 0)
    {
        // A resource teljesen leallt. Ilyenkor a korabbi lifecycle ciklus
        // dependency-hiba jelzese mar nem viheto at egy kesobbi, uj user
        // igenylesre. Az uj igenyles uj lifecycle ciklust kezd.
        Resource_releaseDependencies(resource);
        resource->flags.recoveryActive = false;
        resource->flags.recoveryPending = false;
        resource->flags.recoveryCascadePending = false;
        resource->flags.restartAfterStop = false;
        resource->flags.stoppedDueToDependencyError = false;
        Resource_clearRecoveryRequests(resource);

        // Uj igenyles uj lifecycle ciklust kezd, ezert a regi hiba nem
        // terjedhet tovabb egy kesobbi, mar fuggetlen inditasi kiserletre.
        Resource_clearCurrentErrorInfo(resource);
        resource->flags.faultProcessActive = false;
        resource->recoveryOverride = RESOURCE_RECOVERY_OVERRIDE_DEFAULT;

        ResourceManager_unlock(manager);
        return;
    }

    if ((resource->errorInfo.faultSource == resource) &&
        resource->flags.recoveryPending &&
        !Resource_faultDependentsStopped(resource))
    {
        ResourceManager_unlock(manager);
        return;
    }

    userRecoveryRequested = Resource_hasRecoveryRequest(resource);

    if (userRecoveryRequested)
    {
        resource->flags.recoveryPending = true;
    }

    // A rogzitett hiba a manager recoveryjeig megmarad. A STOPPED callback
    // csak a leallas veget jelzi, nem torli a hiba okat es nem indit ujra.
    if ((resource->errorInfo.status != kStatus_Success) &&
        (resource->errorInfo.resource != NULL) &&
        !resource->flags.recoveryPending)
    {
        Resource_setStateInternal(resource, RESOURCE_ERROR_);
        ResourceManager_unlock(manager);
        return;
    }

    // A sajat igenyles aktiv, ezert a dependency-ket is igenyeljuk.
    Resource_requestDependencies(resource);

    dependenciesReady = Resource_checkDependencies(resource,
                                                   &errorFound,
                                                   &recoveryRequested,
                                                   &dependencyErrorInfo);

    if ((errorFound || recoveryRequested) &&
        (resource->errorInfo.status == kStatus_Success) &&
        (dependencyErrorInfo.resource != NULL))
    {
        Resource_copyErrorInfo(resource, &dependencyErrorInfo);
    }

    if (errorFound)
    {
        if (recoveryRequested)
        {
            Resource_requestDependencyRecovery(resource);
        }

        // Ha mar van explicit recovery igeny, azt meg kell orizni addig,
        // amig a hibas dependency helyre nem all. A dependency-hiba miatt
        // varakozo resource ilyenkor WAITING allapotban marad. Recovery
        // igeny nelkul viszont ERROR allapot a helyes vegallapot.
        if (resource->flags.recoveryPending)
        {
            Resource_setStateInternal(resource,
                                      RESOURCE_WAITING_FOR_DEPENDENCY);
        }
        else
        {
            Resource_setStateInternal(resource, RESOURCE_ERROR_);
        }

        ResourceManager_unlock(manager);
        return;
    }

    if (!dependenciesReady)
    {
        if (recoveryRequested)
        {
            Resource_requestDependencyRecovery(resource);
        }

        if (resource->flags.stoppedDueToDependencyError &&
            resource->flags.hasStarted &&
            !resource->flags.recoveryPending)
        {
            // Egy korabban mukodo resource dependency-hiba miatt nem
            // indulhat ujra automatikusan. Recovery csak explicit user
            // keresre indulhat. Az AUTO_RECOVER csak a dependencyt allitja
            // helyre, a dependent sajat recoveryjet nem.
            Resource_setStateInternal(resource, RESOURCE_ERROR_);
        }
        else
        {
            Resource_setStateInternal(resource,
                                      RESOURCE_WAITING_FOR_DEPENDENCY);
        }

        ResourceManager_unlock(manager);
        return;
    }

    // Ha a resource dependency-hiba miatt allt le, a dependencyk kesobbi
    // helyreallasa onmagaban nem jelenthet automatikus ujrainditast.
    // Recoveryhez explicit user igenyles szukseges.
    if ((resource->flags.stoppedDueToDependencyError) &&
        (resource->flags.hasStarted) &&
        (!resource->flags.recoveryPending))
    {
        Resource_setStateInternal(resource, RESOURCE_ERROR_);
        ResourceManager_unlock(manager);
        return;
    }

    // A dependency-k rendben vannak. Ha korabban recoveryt kert valaki,
    // most indulhat a tenyleges recovery ciklus.
    if (resource->flags.recoveryPending && !resource->flags.recoveryActive)
    {
        if (resource->flags.hasStarted)
        {
            if (!Resource_beginRecovery(resource, true))
            {
                Resource_setInternalError(resource, kStatus_Fail);
                ResourceManager_unlock(manager);
                return;
            }

            // A RECOVERY callback reentrans lehet. A callbackbol a user
            // elengedheti a resource-t, ezert a callback visszaterese utan
            // ujra ellenorizni kell a refcountot, mielott STARTING allapotba
            // tesszuk a resource-t.
            if ((resource->state != RESOURCE_STOPPED) ||
                !resource->flags.recoveryActive ||
                !resource->flags.recoveryPending)
            {
                ResourceManager_unlock(manager);
                return;
            }

            if (resource->refCount == 0)
            {
                resource->flags.recoveryActive = false;
                resource->flags.recoveryPending = false;
                resource->flags.recoveryCascadePending = false;
                resource->flags.restartAfterStop = false;
                Resource_releaseDependencies(resource);
                Resource_setStateInternal(resource, RESOURCE_STOPPED);
                ResourceManager_unlock(manager);
                return;
            }
        }
        else
        {
            // Az elso indulast tovabbra is STARTED esemeny jelzi, de az
            // explicit ujraprobalkozas a recovery limitbe beleszamit.
            if (!Resource_beginRecovery(resource, false))
            {
                Resource_setInternalError(resource, kStatus_Fail);
                ResourceManager_unlock(manager);
                return;
            }
        }
    }

    resource->flags.stoppedDueToDependencyError = false;
    Resource_setStateInternal(resource, RESOURCE_STARTING);
    resource->flags.lifecycleActive = true;
    resource->flags.stopRequestFailed = false;

    ResourceManager_unlock(manager);

    // Lifecycle callback nem mutex alatt fut.
    Resource_dispatchStartRequest(resource);
}
//------------------------------------------------------------------------------
// STARTING allapot feldolgozasa
static void Resource_process_starting(Resource_t* resource)
{
    // STARTING alatt a driver vagy a felhasznalo visszajelzesere varunk.
    // A kovetkezo allapotot az uj jelentes vagy a hiba hatarozza meg.

    ResourceManager_t* manager;
    manager = resource->manager;

    ResourceManager_lock(manager);

    if (resource->refCount == 0)
    {
        resource->flags.restartAfterStop = false;
        Resource_setStateInternal(resource, RESOURCE_STOPPING_);

        ResourceManager_unlock(manager);
        Resource_dispatchStopRequest(resource);
        return;
    }

    // A resource sajat szala a Resource_setState() hivassal mar
    // bejelenthette, hogy elindult. A feldolgozas itt csak az uj allapotot
    // hasznalja, nem pollingolja a statuszt.
    if (resource->state != RESOURCE_STARTING)
    {
        ResourceManager_unlock(manager);
        return;
    }

    ResourceManager_unlock(manager);
}
//------------------------------------------------------------------------------
// RUNNING allapot feldolgozasa
static void Resource_process_running(Resource_t* resource)
{
    // RUNNING alatt a resource aktiv, es a fuggosegek allapotat figyeli.
    // Itt kezeljuk az uj igenyeket, a leallitast es a hibakat is.

    ResourceManager_t* manager = resource->manager;
    bool errorFound;
    bool dependencyRecovery;
    bool dependenciesReady;
    ResourceErrorInfo_t dependencyErrorInfo;

    ResourceManager_lock(manager);

    if (resource->refCount == 0)
    {
        resource->flags.restartAfterStop = false;
        Resource_setStateInternal(resource, RESOURCE_STOPPING_);

        ResourceManager_unlock(manager);
        Resource_dispatchStopRequest(resource);
        return;
    }

    dependenciesReady = Resource_checkDependencies(resource,
                                                   &errorFound,
                                                   &dependencyRecovery,
                                                   &dependencyErrorInfo);

    if (dependencyRecovery)
    {
        Resource_requestDependencyRecovery(resource);
    }

    if (errorFound || !dependenciesReady)
    {
        if ((errorFound || dependencyRecovery) &&
            (dependencyErrorInfo.resource != NULL))
        {
            Resource_copyErrorInfo(resource, &dependencyErrorInfo);
        }

        // A dependency-hibat a sajat usereknek is jelezni kell, mielott
        // a resource leallitasa megkezdodik. Az eredeti hibaforras megmarad.
        if ((errorFound || dependencyRecovery) &&
            (resource->errorInfo.resource != NULL) &&
            !resource->flags.errorEventSent)
        {
            resource->flags.errorEventSent = true;
            Resource_notifyEvent(resource, RESOURCE_EVENT_ERROR,
                                 &resource->errorInfo);

            // A callback reentrans lehet, es megvaltoztathatja az allapotot.
            if (resource->state != RESOURCE_RUNNING)
            {
                ResourceManager_unlock(manager);
                return;
            }
        }

        // Dependency hiba eseten a resource nem maradhat RUNNING.
        // Az AUTO_RECOVER dependency-k recoveryje a dependency sajat
        // recoveryjen keresztul tortenik; a dependent csak akkor indulhat
        // ujra, ha minden szukseges dependency rendben van es recoveryt kert.
        resource->flags.restartAfterStop = false;
        // Csak tenyleges dependency ERROR eseten jeloljuk a leallitast
        // dependency-hiba kovetkezmenyekent. Egy atmeneti, nem hibas
        // dependency allapot miatt nem tilthatjuk meg kesobb az
        // automatikus folytatast.
        // Az AUTO_RECOVER dependency ERROR is valodi dependency-hiba,
        // akkor is, ha a hiba meg recovery elott all.
        // A dependencyRecovery jelzi ezt az esetet.
        resource->flags.stoppedDueToDependencyError =
            errorFound || dependencyRecovery;
        Resource_setStateInternal(resource, RESOURCE_STOPPING_);

        ResourceManager_unlock(manager);
        Resource_dispatchStopRequest(resource);
        return;
    }

    if (!resource->flags.runningEventSent)
    {
        resource->flags.runningEventSent = true;
        resource->flags.faultProcessActive = false;
        resource->flags.recoveryFailed = false;
        resource->recoveryOverride = RESOURCE_RECOVERY_OVERRIDE_DEFAULT;

        if (resource->flags.recoveryActive && resource->flags.hasStarted)
        {
            resource->flags.recoveryActive = false;
            resource->flags.recoveryPending = false;
            resource->flags.recoveryCascadePending = false;
            resource->recoveryAttempts = 0;
            resource->recoveryFaultSource = NULL;
            Resource_notifyEvent(resource, RESOURCE_EVENT_RECOVERED, NULL);
        }
        else
        {
            resource->flags.recoveryActive = false;
            resource->flags.recoveryPending = false;
            resource->flags.recoveryCascadePending = false;
            resource->recoveryAttempts = 0;
            resource->flags.hasStarted = true;
            Resource_clearRecoveryRequests(resource);
            Resource_notifyEvent(resource, RESOURCE_EVENT_STARTED, NULL);
        }

        // Az esemeny callback reentrans lehet, allapotot is valtoztathat.
        // A callback utani regi RUNNING logika ilyenkor ervenytelen.
        if (resource->state != RESOURCE_RUNNING)
        {
            ResourceManager_unlock(manager);
            return;
        }
    }

    // Egy mar mukodo resource-t ujonnan igenylo user csak a sajat
    // callbackjeben kap STARTED vissza jelzest; a korabbi usereket nem
    // ertesitjuk ujra.
    if (resource->state == RESOURCE_RUNNING)
    {
        Resource_notifyPendingStartedUsers(resource);
    }

    ResourceManager_unlock(manager);
}
//------------------------------------------------------------------------------
// WAITING_FOR_DEPENDENCY allapot feldolgozasa
static void Resource_process_waiting(Resource_t* resource)
{
    // A resource egy vagy tobb fuggoseg elerhetosegere var.
    // A manager ujra ellenorzi, amikor valamelyik fuggoseg valtozik.

    ResourceManager_t* manager = resource->manager;
    bool errorFound;
    bool dependencyRecovery;
    bool dependenciesReady;
    bool recoveryStarted = false;
    ResourceErrorInfo_t dependencyErrorInfo;

    ResourceManager_lock(manager);

    if (resource->refCount == 0)
    {
        Resource_releaseDependencies(resource);
        resource->flags.recoveryActive = false;
        resource->flags.recoveryPending = false;
        resource->flags.recoveryCascadePending = false;
        resource->flags.restartAfterStop = false;
        resource->flags.stoppedDueToDependencyError = false;
        Resource_clearRecoveryRequests(resource);
        Resource_setStateInternal(resource, RESOURCE_STOPPED);
        ResourceManager_unlock(manager);
        return;
    }

    Resource_requestDependencies(resource);

    if (Resource_hasRecoveryRequest(resource) &&
        resource->flags.recoveryCascadePending)
    {
        resource->flags.recoveryPending = true;
    }

    dependenciesReady = Resource_checkDependencies(resource,
                                                   &errorFound,
                                                   &dependencyRecovery,
                                                   &dependencyErrorInfo);

    if (dependencyRecovery)
    {
        Resource_requestDependencyRecovery(resource);
    }
    if (resource->flags.recoveryPending && errorFound)
    {
        Resource_requestPropagatedDependencyRecovery(resource);
    }

    // Ha mar nincs aktiv dependenciahiba, a korabban tovabbitott
    // hibainformacio elavult lehet. A sajat hibat viszont meg kell orizni.
    if (!errorFound && !dependencyRecovery &&
        !resource->flags.recoveryPending &&
        (resource->errorInfo.resource != NULL) &&
        (resource->errorInfo.resource != resource))
    {
        ResourceErrorInfo_t noErrorInfo;

        noErrorInfo.status = kStatus_Success;
        noErrorInfo.resource = NULL;
        Resource_copyErrorInfo(resource, &noErrorInfo);
    }

    if (errorFound || !dependenciesReady)
    {
        // Az elso indulaskor az AUTO_RECOVER dependency hibaja meg csak
        // varakozasi ok. Ne kuldjunk ERROR esemenyt, es engedjuk, hogy az
        // elso inditas folytatodjon, ha a dependency helyreallt.
        bool reportError = errorFound ||
                           (dependencyRecovery && resource->flags.hasStarted);

        if (reportError && (dependencyErrorInfo.resource != NULL))
        {
            Resource_copyErrorInfo(resource, &dependencyErrorInfo);
        }

        if (reportError && !resource->flags.errorEventSent)
        {
            resource->flags.errorEventSent = true;
            Resource_notifyEvent(resource, RESOURCE_EVENT_ERROR,
                                 &resource->errorInfo);

            // Az esemeny callback reentrans lehet, es allapotot valtoztathat.
            if (resource->state != RESOURCE_WAITING_FOR_DEPENDENCY)
            {
                ResourceManager_unlock(manager);
                return;
            }
        }

        // A tenylegesen jelentett dependency-hiba tartos. Meglevo explicit
        // recovery keres eseten WAITING maradunk, egyebkent ERROR-ba valtunk.
        if (reportError && !resource->flags.recoveryPending)
        {
            Resource_setStateInternal(resource, RESOURCE_ERROR_);
        }

        ResourceManager_unlock(manager);
        return;
    }

    if (resource->flags.recoveryPending && !resource->flags.recoveryActive)
    {
        if (resource->flags.hasStarted)
        {
            if (!Resource_beginRecovery(resource, true))
            {
                Resource_setInternalError(resource, kStatus_Fail);
                ResourceManager_unlock(manager);
                return;
            }

            recoveryStarted = true;
        }
        else
        {
            // Az elso sikeres indulast STARTED jelzi, de az explicit
            // ujraprobalkozas a recovery limitbe beleszamit.
            if (!Resource_beginRecovery(resource, false))
            {
                Resource_setInternalError(resource, kStatus_Fail);
                ResourceManager_unlock(manager);
                return;
            }
            recoveryStarted = true;
        }

        // A recovery callback reentrans lehet. A callback megvaltoztathatja
        // a resource allapotat, a recovery allapotat vagy a refcountot.
        // Csak akkor kell a recovery utani allapotot ujraellenorizni, ha
        // tenyleges recovery callback futott le.
        if (recoveryStarted &&
            ((resource->state != RESOURCE_WAITING_FOR_DEPENDENCY) ||
             !resource->flags.recoveryActive ||
             !resource->flags.recoveryPending))
        {
            ResourceManager_unlock(manager);
            return;
        }

        if (resource->refCount == 0)
        {
            Resource_releaseDependencies(resource);
            resource->flags.recoveryActive = false;
            resource->flags.recoveryPending = false;
            resource->flags.recoveryCascadePending = false;
            resource->flags.restartAfterStop = false;
            Resource_setStateInternal(resource, RESOURCE_STOPPED);
            ResourceManager_unlock(manager);
            return;
        }
    }

    Resource_setStateInternal(resource, RESOURCE_STARTING);
    resource->flags.lifecycleActive = true;
    resource->flags.stopRequestFailed = false;

    ResourceManager_unlock(manager);
    Resource_dispatchStartRequest(resource);
}
//------------------------------------------------------------------------------
// ERROR allapot feldolgozasa
static void Resource_process_error(Resource_t* resource)
{
    // ERROR alatt a hibafolyamat es a recovery dontesei futnak le.
    // A policy, az override es az autoRecovery egyutt hatarozza meg a lepest.

    ResourceManager_t* manager = resource->manager;
    bool doStop = false;
    bool retryWithoutStop = false;
    bool errorFound;
    bool dependenciesReady;
    bool dependencyRecovery;
    bool userRecoveryRequested;
    bool recoveryStarted = false;
    ResourceErrorInfo_t dependencyErrorInfo;

    ResourceManager_lock(manager);

    if (!resource->flags.errorEventSent)
    {
        resource->flags.errorEventSent = true;
        Resource_notifyEvent(resource, RESOURCE_EVENT_ERROR,
                                 &resource->errorInfo);

        // Az ERROR esemeny callback reentrans lehet. Ha a callback mas
        // allapotba vitte a resource-t, ezt a feldolgozast nem szabad tovabb
        // folytatni, mert mar egy masik allapot logikajat irnank felul.
        if (resource->state != RESOURCE_ERROR_)
        {
            ResourceManager_unlock(manager);
            return;
        }
    }

    // ERROR eseten a futo lifecycle-t mindenkeppen le kell allitani.
    // A recovery igeny es a dependency-k allapota a STOPPED feldolgozasakor
    // ujra ellenorizheto; addig az eredeti errorInfo valtozatlan marad.
    if (resource->flags.lifecycleActive)
    {
        if (resource->flags.stopRequestFailed)
        {
            // A stop kerelem callback hibazott; ne hivjuk ujra ciklikusan.
            ResourceManager_unlock(manager);
            return;
        }

        if (Resource_hasRecoveryRequest(resource))
        {
            resource->flags.recoveryPending = true;
        }

        Resource_setStateInternal(resource, RESOURCE_STOPPING_);
        ResourceManager_unlock(manager);
        Resource_dispatchStopRequest(resource);
        return;
    }

    if (resource->refCount == 0)
    {
        Resource_clearRecoveryRequests(resource);
        Resource_releaseDependencies(resource);
        resource->flags.restartAfterStop = false;
        resource->flags.recoveryActive = false;
        resource->flags.recoveryPending = false;
        resource->flags.recoveryCascadePending = false;

        if (resource->flags.lifecycleActive)
        {
            Resource_setStateInternal(resource, RESOURCE_STOPPING_);
            doStop = true;
        }
        else
        {
            Resource_setStateInternal(resource, RESOURCE_STOPPED);
        }
    }
    else
    {
        if ((resource->errorInfo.faultSource == resource) &&
            resource->flags.recoveryPending &&
            !Resource_faultDependentsStopped(resource))
        {
            ResourceManager_unlock(manager);
            return;
        }

        userRecoveryRequested = Resource_hasRecoveryRequest(resource);

        // A user recoverykerelmet meg kell orizni mindaddig, amig a
        // dependency-k is helyre nem allnak.
        if (userRecoveryRequested)
        {
            resource->flags.recoveryPending = true;
        }

        Resource_requestDependencies(resource);

        dependenciesReady = Resource_checkDependencies(resource,
                                                       &errorFound,
                                                       &dependencyRecovery,
                                                       &dependencyErrorInfo);

        if (dependencyRecovery)
        {
            Resource_requestDependencyRecovery(resource);
        }
        if (resource->flags.recoveryPending && errorFound)
        {
            Resource_requestPropagatedDependencyRecovery(resource);
        }

        if (errorFound || !dependenciesReady)
        {
            // A sajat ERROR esemeny mar kiment, ezert az eredeti hibainformacio
            // marad ervenyben a sajat recovery varakozasa alatt.
            // A PROPAGATE hiba vagy a nem kesz dependency megakadalyozza
            // a sajat recoveryt. Az AUTO_RECOVER dependency recoveryje ettol
            // fuggetlenul elindulhat, de a dependent recovery esemenyet meg
            // nem kuldjuk ki. A user recovery kerelmeit addig meg kell orizni,
            // amig a tenyleges recovery el nem indul.
            ResourceManager_unlock(manager);
            return;
        }

        // Minden dependency rendben van. Most mar tenylegesen elindithato a
        // korabban kert recovery, ha volt user vagy AUTO_RECOVER igeny.
        if (resource->flags.recoveryPending)
        {

            // Az elso indulast nem szabad recoverykent kezelni.
            // Ilyenkor a recovery kerelem csak az inditasi szandekot jelenti,
            // az elso sikeres RUNNING esemeny pedig STARTED lesz.
            // Ez arra is ervenyes, ha az elso inditasi kiserlet
            // ERROR-ba futott,
            // es a user utana explicit recoveryt kert.
            if (!resource->flags.hasStarted)
            {
                // Az elso indulasi kiserlet utan kert explicit ujrainditas
                // recovery probalkozasnak szamit, de nem kuld RECOVERY
                // esemenyt. A sikeres RUNNING tovabbra is STARTED lesz.
                if (!resource->flags.recoveryActive)
                {
                    if (!Resource_beginRecovery(resource, false))
                    {
                        ResourceManager_unlock(manager);
                        return;
                    }

                    recoveryStarted = true;
                }
            }
            else
            {
                Resource_clearRecoveryRequests(resource);
                resource->flags.recoveryPending = true;

                if (!resource->flags.recoveryActive)
                {
                    if (!Resource_beginRecovery(resource, true))
                    {
                        ResourceManager_unlock(manager);
                        return;
                    }

                    recoveryStarted = true;
                }
            }

            // A recovery callback reentrans lehet.
            // Az ellenorzest csak akkor kell elvegezni, ha tenyleges recovery
            // callback futott le. Az elso indulaskor nincs ilyen callback.
            if (recoveryStarted &&
                ((resource->state != RESOURCE_ERROR_) ||
                 !resource->flags.recoveryActive ||
                 !resource->flags.recoveryPending))
            {
                ResourceManager_unlock(manager);
                return;
            }

            if (resource->refCount == 0)
            {
                resource->flags.restartAfterStop = false;
                resource->flags.recoveryActive = false;
                resource->flags.recoveryPending = false;
                resource->flags.recoveryCascadePending = false;
                Resource_releaseDependencies(resource);

                if (resource->flags.lifecycleActive)
                {
                    Resource_setStateInternal(resource, RESOURCE_STOPPING_);
                    doStop = true;
                }
                else
                {
                    Resource_setStateInternal(resource, RESOURCE_STOPPED);
                }
            }
            else if (resource->flags.lifecycleActive)
            {
                resource->flags.restartAfterStop = true;
                Resource_setStateInternal(resource, RESOURCE_STOPPING_);
                doStop = true;
            }
            else
            {
                Resource_setStateInternal(resource, RESOURCE_STARTING);
                resource->flags.lifecycleActive = true;
                retryWithoutStop = true;
            }
        }
        else
        {
            // Sikertelen recovery utan nincs uj recovery igeny: maradunk ERROR
            // allapotban, amig user recoveryt nem ker vagy dependency hiba nem
            // valt ki uj AUTO_RECOVER ciklust.
            Resource_clearRecoveryRequests(resource);
        }
    }

    ResourceManager_unlock(manager);

    if (doStop)
    {
        Resource_dispatchStopRequest(resource);
    }
    else if (retryWithoutStop)
    {
        Resource_dispatchStartRequest(resource);
    }
}
//------------------------------------------------------------------------------
// STOPPING allapot kezelese
static void Resource_process_stopping(Resource_t* resource)
{
    // STOPPING alatt megvarjuk a leallitas befejezeset.
    // A fuggosegi eroforrasok igenyeit csak a megfelelo ponton engedjuk el.

    ResourceManager_t* manager = resource->manager;
    bool errorFound;
    bool dependencyRecovery;
    ResourceErrorInfo_t dependencyErrorInfo;

    ResourceManager_lock(manager);

    // A callback mar kozben jelezhette a STOPPED allapotot.
    if (resource->state != RESOURCE_STOPPING_)
    {
        ResourceManager_unlock(manager);
        return;
    }

    // Leallas kozben is vizsgalni kell a dependency-ket, mert egy
    // dependenciahiba a stop callback inditasa utan is megerkezhet.
    (void)Resource_checkDependencies(resource,
                                     &errorFound,
                                     &dependencyRecovery,
                                     &dependencyErrorInfo);

    // A dependency hiba miatt torteno leallast akkor is meg kell orizni,
    // ha a hiba a STOPPING allapot alatt erkezett. Sajat, mar rogzitett
    // hibat azonban nem szabad dependency hibara cserelni.
    if (errorFound || dependencyRecovery)
    {
        if ((resource->errorInfo.status == kStatus_Success) &&
            (dependencyErrorInfo.resource != NULL))
        {
            Resource_copyErrorInfo(resource, &dependencyErrorInfo);
        }

        if (resource->errorInfo.resource != resource)
        {
            resource->flags.stoppedDueToDependencyError = true;
        }
    }

    if (dependencyRecovery)
    {
        Resource_requestDependencyRecovery(resource);
    }

    // A hibat meg a STOPPED allapot esemenye elott jelezzuk a usereknek.
    // Normal leallitasnal nincs errorFound, igy nem keletkezik ERROR.
    if ((errorFound || dependencyRecovery ||
         ((resource->errorInfo.status != kStatus_Success) &&
          (resource->errorInfo.resource == resource))) &&
        (resource->errorInfo.status != kStatus_Success) &&
        (resource->errorInfo.resource != NULL) &&
        !resource->flags.errorEventSent)
    {
        resource->flags.errorEventSent = true;
        Resource_notifyEvent(resource, RESOURCE_EVENT_ERROR,
                             &resource->errorInfo);

        // A callback reentrans lehet, es allapotot valtoztathat.
        if (resource->state != RESOURCE_STOPPING_)
        {
            ResourceManager_unlock(manager);
            return;
        }
    }

    ResourceManager_unlock(manager);
}
//------------------------------------------------------------------------------
// DONE allapot feldolgozasa. A resource magatol befejezodott, stop callback
// nem hivhato. A sajat dependency-k mar nem szuksegesek.
static void Resource_process_done(Resource_t* resource)
{
    // DONE jelzi, hogy a resource mukodese veglegesen befejezodott.
    // Innen a manager nem inditja ujra automatikusan a resource-t.

    ResourceManager_t* manager = resource->manager;

    ResourceManager_lock(manager);

    if (resource->state != RESOURCE_DONE_)
    {
        ResourceManager_unlock(manager);
        return;
    }

    Resource_releaseDependencies(resource);

    if (!resource->flags.doneEventSent)
    {
        resource->flags.doneEventSent = true;
        Resource_notifyEvent(resource, RESOURCE_EVENT_DONE, NULL);

        // Az esemeny callback megvaltoztathatja az allapotot vagy a refcountot.
        if (resource->state != RESOURCE_DONE_)
        {
            ResourceManager_unlock(manager);
            return;
        }
    }

    // A DONE esemeny kikuldese kozben is erkezhet keres, peldaul egy korabban
    // sorra
    // kerulo user callbackjebol. A fuggoben levo DONE jelzeseket elobb ki kell
    // kuldeni,
    // es csak utana donthetunk az eroforras ujrahasznalatarol.
    Resource_notifyPendingDoneUsers(resource);
    if (resource->state != RESOURCE_DONE_)
    {
        ResourceManager_unlock(manager);
        return;
    }

    // Ha mar nincs igenylo, a kovetkezo request uj muveletet indithat.
    // STOPPED esemenyt nem kuldunk, mert nem tortent leallitas.
    if (resource->refCount == 0)
    {
        resource->flags.recoveryActive = false;
        resource->flags.recoveryPending = false;
        resource->flags.recoveryCascadePending = false;
        resource->flags.restartAfterStop = false;
        resource->flags.stoppedDueToDependencyError = false;
        resource->flags.hasStarted = false;
        Resource_clearCurrentErrorInfo(resource);
        resource->flags.faultProcessActive = false;
        resource->recoveryOverride = RESOURCE_RECOVERY_OVERRIDE_DEFAULT;

        Resource_setStateInternal(resource, RESOURCE_STOPPED);
        resource->flags.stoppedEventSent = true;
    }

    ResourceManager_unlock(manager);
}
//------------------------------------------------------------------------------
// Egyetlen resource feldolgozasa. Allapot szerinti kiertekelo fuggveny
// hivasa
static void Resource_process(Resource_t* resource)
{
    // Egy feldolgozasi korben az aktualis allapothoz tartozo logika fut le.
    // Az allapotvaltas uj munkat utemezhet a pending listan.

    ResourceManager_t* manager;
    ResourceState_t state;
    manager = resource->manager;

    ResourceManager_lock(manager);
    state = resource->state;
    ResourceManager_unlock(manager);

    switch (state)
    {
        case RESOURCE_STOPPED:
            Resource_process_stopped(resource);
            break;

        case RESOURCE_STARTING:
            Resource_process_starting(resource);
            break;

        case RESOURCE_RUNNING:
            Resource_process_running(resource);
            break;

        case RESOURCE_STOPPING_:
            Resource_process_stopping(resource);
            break;

        case RESOURCE_ERROR_:
            Resource_process_error(resource);
            break;

        case RESOURCE_WAITING_FOR_DEPENDENCY:
            Resource_process_waiting(resource);
            break;

        case RESOURCE_DONE_:
            Resource_process_done(resource);
            break;

        default:
            ResourceManager_lock(manager);
            Resource_setInternalError(resource, kStatus_Fail);
            ResourceManager_unlock(manager);
            break;
    }
}
//------------------------------------------------------------------------------
// Ellenorzi, hogy elertuk-e az osszes-eroforras-leallt allapotot. A callback
// a mutexen kivul fut, hogy a leallasi logika ne tartsa fogva a manager lockot.
static void ResourceManager_notifyAllStoppedIfNeeded(ResourceManager_t* manager)
{
    // Egyszer jelez, ha a manager minden eroforrasa leallt.
    // A visszahivas csak a teljes leallasi feltetel teljesulese utan futhat.

    ResourceManager_all_stopped_cb_t callback = NULL;
    void* context = NULL;
    Resource_t* resource;
    bool allStopped = true;

    ResourceManager_lock(manager);

    if (manager->allStoppedCallbackArmed &&
        (manager->allStoppedCallback != NULL) &&
        (manager->pendingResources == NULL))
    {
        for (resource = manager->resources;
             resource != NULL;
             resource = resource->next)
        {
            // RefCount vizsgalat is kell: egy friss request mar aktiv igeny,
            // akkor is, ha a resource a manager feldolgozasa elott meg STOPPED.
            if ((resource->state != RESOURCE_STOPPED) ||
                (resource->refCount != 0U) ||
                resource->flags.lifecycleActive)
            {
                allStopped = false;
                break;
            }
        }

        if (allStopped)
        {
            // Edge-triggered jelzes: ujra csak akkor hivjuk meg, ha valamelyik
            // resource kesobb ismet nem-STOPPED allapotba lepett.
            manager->allStoppedCallbackArmed = false;
            callback = manager->allStoppedCallback;
            context = manager->allStoppedCallbackData;
        }
    }

    ResourceManager_unlock(manager);

    if (callback != NULL)
    {
        callback(manager, context);
    }
}
//------------------------------------------------------------------------------
// Eroforras-esemeny kuldese taskbol.
// Alkalmazasi esemenyek kuldese task kontextusbol.
// A ResourceManager az egyetlen publikus belepesi pont: loophoz rendelt
// resource eseten tovabbitja a ResourceLoop fele, egyebkent a manager task
// dolgozza fel az opcionalis eventFunc callback segitsegevel.
void Resource_setEvent(Resource_t* resource, uint64_t events)
{
    ResourceLoopExtension_t* loopExtension;

    assert(resource);
    loopExtension = resource->loopExtension;

    // Ures maszk nem jelent feldolgozando munkat, ezert nem ebresztunk taskot.
    if (events == 0U)
    {
        return;
    }

    // A vegrehajtasi helyet a loop pointer hatarozza meg. A manager itt csak
    // tovabbitja a kerest; alkalmazasi callbacket nem futtat kozvetlenul.
    if ((loopExtension != NULL) &&
        (loopExtension->loop != NULL))
    {
        ResourceLoop_setEvent(resource, events);
        return;
    }

    taskENTER_CRITICAL();
    resource->inputEventsAsync |= events;
    taskEXIT_CRITICAL();

    if (resource->manager != NULL)
    {
        (void)xSemaphoreGive(resource->manager->wakeup);
    }
}
//------------------------------------------------------------------------------
// Eroforras-esemeny kuldese ISR-bol.
// ISR-bol kuldott alkalmazasi esemeny.
// Ebben a kontextusban nem futtathato callback, csak az esemeny rogzitese es
// a megfelelo task ebresztese. A loopos es loop nelkuli utakhoz is ISR-biztos
// jelzesi mechanizmust kell hasznalni.
void Resource_setEventFromIsr(Resource_t* resource, uint64_t events)
{
    ResourceLoopExtension_t* loopExtension;

    BaseType_t higherPriorityTaskWoken = pdFALSE;
    UBaseType_t interruptStatus;

    assert(resource);
    loopExtension = resource->loopExtension;

    // Ures maszk nem jelent feldolgozando munkat, ezert nem ebresztunk taskot.
    if (events == 0U)
    {
        return;
    }

    // A vegrehajtasi helyet a loop pointer hatarozza meg. A manager itt csak
    // tovabbitja a kerest; alkalmazasi callbacket nem futtat kozvetlenul.
    if ((loopExtension != NULL) &&
        (loopExtension->loop != NULL))
    {
        ResourceLoop_setEventFromIsr(resource, events);
        return;
    }

    interruptStatus = taskENTER_CRITICAL_FROM_ISR();
    resource->inputEventsAsync |= events;
    taskEXIT_CRITICAL_FROM_ISR(interruptStatus);

    if (resource->manager != NULL)
    {
        (void)xSemaphoreGiveFromISR(resource->manager->wakeup,
                                    &higherPriorityTaskWoken);
        portYIELD_FROM_ISR(higherPriorityTaskWoken);
    }
}
//------------------------------------------------------------------------------
// Manager feldolgozo rutinja.
// Addig dolgozik, amig van pending resource, majd var a kovetkezo esemenyre.
// Feldolgozza a pending listat, majd var a kovetkezo ebresztesre.
void ResourceManager_run(ResourceManager_t* manager)
{
    // A manager fo ciklusa a pending listan levo resource-okat dolgozza fel.
    // A szemafor csak ebresztesre kell; a lista tartalmazza a munkaallapotot.

    assert(manager);

    // Hiba, ha a manager meg nincs initelve
    if (!manager->initialized)
    {
        return;
    }

    ResourceManager_lock(manager);
    manager->running = true;
    ResourceManager_unlock(manager);

    Resource_t* resrce;

    for (;;)
    {
        // Amig van feldolgozando resource, addig nem alszunk el.
        // Az ujra pendingre kerulo resource ugyanebben a ciklusban
        // feldolgozhato.
        for (;;)
        {
            // A pending lista a tenyleges munkasor; a semaphore csak azt jelzi,
            // hogy ujra meg kell vizsgalni a sort. A lock alatt vesszuk le az
            // elemet, majd a feldolgozast a lockon kivul vegezzuk.
            ResourceManager_lock(manager);
            resrce = ResourceManager_popPendingResource(manager);
            ResourceManager_unlock(manager);

            if (resrce == NULL)
            {
                break;
            }

            Resource_process(resrce);

            // Minden feldolgozas utan vizsgaljuk az osszes-leallt feltetelt.
            // Ha tovabbi pending munka van, a helper var a teljes
            // feldolgozasig.
            ResourceManager_notifyAllStoppedIfNeeded(manager);
        }

        // A lifecycle allapotgepek utan feldolgozzuk a loop nelkuli resource-ok
        // esemenyeit. A callback ujabb lifecycle muveletet vagy esemenyt is
        // kerhet; az ilyen kerest a kovetkezo ciklus dolgozza fel.
        ResourceManager_processResourceEvents(manager);

        // Callback alatt ujabb esemeny is erkezhetett, amely ujra feladta
        // a szemafort. A kovetkezo kor elobb a pending listat dolgozza fel.
        (void)xSemaphoreTake(manager->wakeup, portMAX_DELAY);
    }
}
//------------------------------------------------------------------------------
// Megkeresi, hogy egy user a resource valamelyik belso dependency-usere-e.
// A manager mutexet a hivo tartja.
static ResourceDependency_t* ResourceManager_findUserDependency(
    Resource_t* resource, ResourceUser_t* user)
{
    ResourceDependency_t* dependency;

    for (dependency = resource->dependents; dependency != NULL;
         dependency = dependency->nextDependent)
    {
        if (&dependency->internalUser == user)
        {
            return dependency;
        }
    }

    return NULL;
}
//------------------------------------------------------------------------------
// Egy resource osszes userenek kiirasa, attol fuggetlenul, hogy az adott
// user pillanatnyilag igenyli-e az eroforrast. A kulso usernek nincs sajat
// ResourceState_t allapota; nala a requested es recoveryRequested flag latszik.
// A belso dependency-usernel a fuggo resource allapota is megjelenik.
// A manager mutexet a hivo tartja.
static void ResourceManager_printUsers(Resource_t* resource)
{
    // Kiirja az adott resource-hoz regisztralt usereket kulon sorokban.
    // A sorban az igenylesi es recovery jelzesek is megjelennek.

    ResourceUser_t* user;
    bool first = true;

    printf("    Users:\n");

    for (user = resource->users; user != NULL; user = user->next)
    {
        ResourceDependency_t* dependency;
        const char* userName = user->name != NULL ? user->name : "<unnamed>";
        int requested = user->requested ? 1 : 0;
        int recoveryRequested = user->recoveryRequested ? 1 : 0;
        int autoRecovery = user->autoRecovery ? 1 : 0;

        first = false;
        dependency = ResourceManager_findUserDependency(resource, user);
        if (dependency != NULL)
        {
            printf("        %s (%s, requested: %d recoveryRequested: %d "
                   "autoRecovery: %d)\n",
                   userName,
                   Resource_stateTosStr(dependency->dependent->state),
                   requested,
                   recoveryRequested,
                   autoRecovery);
        }
        else
        {
            printf("        %s (User, requested: %d recoveryRequested: %d "
                   "autoRecovery: %d)\n",
                   userName,
                   requested,
                   recoveryRequested,
                   autoRecovery);
        }
    }

    if (first)
    {
        printf("        <none>\n");
    }
}
//------------------------------------------------------------------------------
// Egy resource dependency-listajanak kiirasa. A manager mutexet a hivo tartja.
static void ResourceManager_printDependencies(Resource_t* resource)
{
    // Felsorolja a resource kozvetlen fuggosegeit es azok allapotat.
    // Ez diagnosztika; a fuggosegek mukodeset nem modositja.

    ResourceDependency_t* dependency;
    bool first = true;

    printf("    Dependencies:\n");

    for (dependency = resource->dependencies; dependency != NULL;
         dependency = dependency->next)
    {
        first = false;
        printf("        %s (%s)\n",
               dependency->resource->name,
               Resource_stateTosStr(dependency->resource->state));
    }

    if (first)
    {
        printf("        <none>\n");
    }
}
//------------------------------------------------------------------------------
// Az osszes eroforras allapotanak es recovery-informacioinak kiirasa
// printf-fel.
// verbose=true eseten az osszes usert (a request flagjeikkel) es a fuggosegek
// listajat is kiirja.
// A mutex a konzisztens snapshot miatt a teljes konzolkiiras alatt zarva marad.
void ResourceManager_printStatus(ResourceManager_t* manager, bool verbose)
{
    // Osszefoglalja a resource allapotat es a kapcsolodo objektumokat.
    // A verbose mod adja hozza a reszletes hibakezelasi mezoket.

    Resource_t* resource;
    bool found = false;

    assert(manager);

    if (!manager->initialized)
    {
        printf("ResourceManager is not initialized.\n");
        return;
    }

    ResourceManager_lock(manager);

    printf("\nResourceManager status\n");
    printf("%-24s %-24s %-16s %8s %-25s %-12s\n",
           "RESOURCE", "STATE", "LOOP", "REFCOUNT",
           "RECOVERY (A/P tries/max)", "LAST_ERROR");
    printf("------------------------------------------------------------"
           "--------------------\n");

    for (resource = manager->resources; resource != NULL;
         resource = resource->next)
    {
        ResourceLoopExtension_t* loopExtension = resource->loopExtension;
        char recoveryText[40];
        char errorText[16];

        found = true;

        if (resource->recoveryMaxAttempts == 0)
        {
            (void)snprintf(recoveryText, sizeof(recoveryText), "%c/%c %lu/inf",
                           resource->flags.recoveryActive ? 'Y' : 'N',
                           resource->flags.recoveryPending ? 'Y' : 'N',
                           (unsigned long)resource->recoveryAttempts);
        }
        else
        {
            (void)snprintf(recoveryText, sizeof(recoveryText), "%c/%c %lu/%lu",
                           resource->flags.recoveryActive ? 'Y' : 'N',
                           resource->flags.recoveryPending ? 'Y' : 'N',
                           (unsigned long)resource->recoveryAttempts,
                           (unsigned long)resource->recoveryMaxAttempts);
        }

        if (resource->lastErrorStatus == kStatus_Success)
        {
            (void)snprintf(errorText, sizeof(errorText), "-");
        }
        else
        {
            (void)snprintf(errorText, sizeof(errorText), "0x%08lX",
                           (unsigned long)(uint32_t)resource->lastErrorStatus);
        }

        printf("%-24.24s %-24.24s %-16.16s %8lu %-25.25s %-12.12s\n",
               resource->name,
               Resource_stateTosStr(resource->state),
               ((loopExtension != NULL) &&
                (loopExtension->loop != NULL)) ?
                   loopExtension->loop->name : "-",
               (unsigned long)resource->refCount,
               recoveryText,
               errorText);
        if (verbose)
        {
            printf("    StartReason=%s RecoveryReason=%s RecoveryFailed=%d "
                   "Policy=%s Override=%s LastOverride=%s\n",
                   (resource->startReason == RESOURCE_START_REASON_RECOVERY) ?
                       "RECOVERY" : "NORMAL",
                   (resource->recoveryReason ==
                    RESOURCE_RECOVERY_REASON_SELF_FAULT) ?
                       "SELF_FAULT" :
                   (resource->recoveryReason ==
                    RESOURCE_RECOVERY_REASON_DEPENDENCY) ?
                       "DEPENDENCY" :
                   (resource->recoveryReason ==
                    RESOURCE_RECOVERY_REASON_HIGHER_LAYER_REQUEST) ?
                       "HIGHER_LAYER_REQUEST" : "NONE",
                   resource->flags.recoveryFailed ? 1 : 0,
                   (resource->recoveryPolicy == RESOURCE_RECOVERY_POLICY_AUTO) ?
                       "AUTO" : "STOP",
                   (resource->recoveryOverride ==
                    RESOURCE_RECOVERY_OVERRIDE_FORCED) ? "FORCED" :
                   (resource->recoveryOverride ==
                    RESOURCE_RECOVERY_OVERRIDE_DISABLED) ?
                       "DISABLED" : "DEFAULT",
                   (resource->lastErrorInfo.recoveryOverride ==
                    RESOURCE_RECOVERY_OVERRIDE_FORCED) ? "FORCED" :
                   (resource->lastErrorInfo.recoveryOverride ==
                    RESOURCE_RECOVERY_OVERRIDE_DISABLED) ?
                       "DISABLED" : "DEFAULT");
        }
        // Az aktiv hibat kulon irjuk ki a tartosan megorzott utolso hibatol.
        // Sikeres helyreallas utan is megmaradhat az utolso hiba diagnosztikai
        // celra.
        if (resource->errorInfo.status != kStatus_Success)
        {
            printf("    CurrentFault=%s Status=0x%08lX "
                   "ReportedBy=%s FailedDependency=%s "
                   "ErrorReason=%s Override=%s\n",
                   (resource->errorInfo.faultSource != NULL) ?
                       resource->errorInfo.faultSource->name : "<none>",
                   (unsigned long)(uint32_t)resource->errorInfo.status,
                   (resource->errorInfo.reportedBy != NULL) ?
                       resource->errorInfo.reportedBy->name : "<none>",
                   (resource->errorInfo.failedDependency != NULL) ?
                       resource->errorInfo.failedDependency->name : "<none>",
                   (resource->errorInfo.reason ==
                    RESOURCE_ERROR_REASON_SELF_FAULT) ? "SELF_FAULT" :
                   (resource->errorInfo.reason ==
                    RESOURCE_ERROR_REASON_DEPENDENCY_FAULT) ?
                       "DEPENDENCY_FAULT" :
                   (resource->errorInfo.reason ==
                    RESOURCE_ERROR_REASON_REPORTED_FAULT) ?
                       "REPORTED_FAULT" : "NONE",
                   (resource->errorInfo.recoveryOverride ==
                    RESOURCE_RECOVERY_OVERRIDE_FORCED) ? "FORCED" :
                   (resource->errorInfo.recoveryOverride ==
                    RESOURCE_RECOVERY_OVERRIDE_DISABLED) ?
                       "DISABLED" : "DEFAULT");
        }

        if (resource->lastErrorInfo.status != kStatus_Success)
        {
            printf("    LastFault=%s Status=0x%08lX "
                   "ReportedBy=%s FailedDependency=%s "
                   "ErrorReason=%s Override=%s\n",
                   (resource->lastErrorInfo.faultSource != NULL) ?
                       resource->lastErrorInfo.faultSource->name : "<none>",
                   (unsigned long)(uint32_t)resource->lastErrorInfo.status,
                   (resource->lastErrorInfo.reportedBy != NULL) ?
                       resource->lastErrorInfo.reportedBy->name : "<none>",
                   (resource->lastErrorInfo.failedDependency != NULL) ?
                       resource->lastErrorInfo.failedDependency->name :
                       "<none>",
                   (resource->lastErrorInfo.reason ==
                    RESOURCE_ERROR_REASON_SELF_FAULT) ? "SELF_FAULT" :
                   (resource->lastErrorInfo.reason ==
                    RESOURCE_ERROR_REASON_DEPENDENCY_FAULT) ?
                       "DEPENDENCY_FAULT" :
                   (resource->lastErrorInfo.reason ==
                    RESOURCE_ERROR_REASON_REPORTED_FAULT) ?
                       "REPORTED_FAULT" : "NONE",
                   (resource->lastErrorInfo.recoveryOverride ==
                    RESOURCE_RECOVERY_OVERRIDE_FORCED) ? "FORCED" :
                   (resource->lastErrorInfo.recoveryOverride ==
                    RESOURCE_RECOVERY_OVERRIDE_DISABLED) ?
                       "DISABLED" : "DEFAULT");
        }

        if (verbose)
        {
            ResourceManager_printUsers(resource);
            printf("\n");
            ResourceManager_printDependencies(resource);
        }
    }

    if (!found)
    {
        printf("<no resources>\n");
    }

    ResourceManager_unlock(manager);
}
//------------------------------------------------------------------------------
// Resource allapot lekerdezese.
ResourceState_t Resource_getState(Resource_t* resource)
{
    // Biztonsagosan lekerdezi a resource aktualis allapotat.

    assert(resource);
    ResourceState_t state;

    ResourceManager_t* manager = resource->manager;
    assert(manager);

    ResourceManager_lock(manager);
    state = resource->state;
    ResourceManager_unlock(manager);

    return state;
}
//------------------------------------------------------------------------------
// Resource refcount lekerdezese. (aktiv igenylok szama)
uint32_t Resource_getRefCount(Resource_t* resource)
{
    // Visszaadja az aktiv igenylesek referencia-szamat.

    assert(resource);
    ResourceManager_t* manager = resource->manager;
    assert(manager);

    uint32_t refCount;
    ResourceManager_lock(manager);
    refCount = resource->refCount;
    ResourceManager_unlock(manager);

    return refCount;
}
//------------------------------------------------------------------------------
// User request allapot lekerdezese.
bool ResourceUser_isRequested(ResourceUser_t* user)
{
    // Megadja, hogy a user jelenleg igenyli-e a resource-t.

    assert(user);
    assert(user->resource);
    ResourceManager_t* manager = user->resource->manager;
    assert(manager);
    bool requested;

    ResourceManager_lock(manager);
    requested = user->requested;
    ResourceManager_unlock(manager);

    return requested;
}
//------------------------------------------------------------------------------
// User recovery igenyles lekerdezese.
bool ResourceUser_recoveryRequested(ResourceUser_t* user)
{
    // Megadja, hogy a user recovery kerese aktiv-e.

    assert(user);
    assert(user->resource);
    ResourceManager_t* manager = user->resource->manager;
    assert(manager);

    bool recoveryRequested;
    ResourceManager_lock(manager);
    recoveryRequested = user->recoveryRequested;
    ResourceManager_unlock(manager);

    return recoveryRequested;
}
//------------------------------------------------------------------------------
