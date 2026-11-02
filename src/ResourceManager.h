//------------------------------------------------------------------------------
// Copyright (c) 2026 [Istvan Suto]
//
// SPDX-License-Identifier: MIT
//------------------------------------------------------------------------------
//------------------------------------------------------------------------------
//  Eroforras kezelo szolgaltatas
//
//    File: ResourceManager.h
//------------------------------------------------------------------------------
#ifndef RESOURCEMANAGER_H_
#define RESOURCEMANAGER_H_

#include <stdbool.h>
#include <stdint.h>
#include "MyCommon.h"
#include "FreeRTOS.h"
#include "semphr.h"

#if !configSUPPORT_STATIC_ALLOCATION
#error "ResourceManager requires configSUPPORT_STATIC_ALLOCATION=1"
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _ResourceManager ResourceManager_t;
typedef struct _Resource Resource_t;
typedef struct _ResourceUser ResourceUser_t;
typedef struct _ResourceDependency ResourceDependency_t;
typedef struct _ResourceObserver ResourceObserver_t;
typedef struct ResourceLoop ResourceLoop_t;
//------------------------------------------------------------------------------
// Hiba eredete egy adott resource nezopontjabol.
typedef enum
{
    // Nincs hiba
    RESOURCE_ERROR_REASON_NONE = 0,
    // Sajat maga jelentett hibat
    RESOURCE_ERROR_REASON_SELF_FAULT,
    // Valamelyik fuggosege hibazik
    RESOURCE_ERROR_REASON_DEPENDENCY_FAULT,
    // Egy magasabb szintrol jeleztek, hogy hibas
    RESOURCE_ERROR_REASON_REPORTED_FAULT
} ResourceErrorReason_t;
//------------------------------------------------------------------------------
// Inditas oka, a start callbackbol kerdezheto le.
typedef enum
{
    // Normal inditas
    RESOURCE_START_REASON_NORMAL = 0,
    // Hiba miatti automatikus ujrainditas miatt hivodott a start
    RESOURCE_START_REASON_RECOVERY
} ResourceStartReason_t;
//------------------------------------------------------------------------------
// A recovery oka.
typedef enum
{
    // Nincs helyreallitasi hiba
    RESOURCE_RECOVERY_REASON_NONE = 0,
    // Sajat hibaja miatt kell recovery
    RESOURCE_RECOVERY_REASON_SELF_FAULT,
    // Valamelyik fuggosege miatt van recovery
    RESOURCE_RECOVERY_REASON_DEPENDENCY,
    // Egy magasabb szintrol jott a hiba jelzes, ami miatt recovery kell
    RESOURCE_RECOVERY_REASON_HIGHER_LAYER_REQUEST
} ResourceRecoveryReason_t;
//------------------------------------------------------------------------------
// Hibajelzeshez tartozo recovery-strategia felulbiralasa.
// A prioritasi sorrend szandekosan a numerikus ertekek sorrendje.
// Ha tobb helyrol erkezik hibajelzes, a legmagasabb prioritasu strategia
// lesz ervenyes az eroforrason.
typedef enum
{
    // Nincs felulbiralas. A recovery az eroforras letrehozasakor beallitott
    // strategia szerint tortenik.
    RESOURCE_RECOVERY_OVERRIDE_DEFAULT = 0,
    // Tiltja a recoveryt a megjelolt eroforrson
    RESOURCE_RECOVERY_OVERRIDE_DISABLED,
    // Mindenkeppen keri a helyreallitast az eroforrason
    RESOURCE_RECOVERY_OVERRIDE_FORCED
} ResourceRecoveryOverride_t;
//------------------------------------------------------------------------------
// A hibaforras alapertelmezett recovery-strategiaja.
typedef enum
{
    // Hiba eseten az eroforras alljon le, es maradjon leallitott allapotban
    RESOURCE_RECOVERY_POLICY_STOP = 0,
    // Hiba miatti leallas utan automatikusan induljon ujra az eroforras
    RESOURCE_RECOVERY_POLICY_AUTO
} ResourceRecoveryPolicy_t;
//------------------------------------------------------------------------------
// Eroforras hibaja eseten kozolt hiba informacio
typedef struct
{
    // A hiba status kodja.
    status_t status;

    // Kompatibilitasi alias a faultSource-ra.
    // Reszletes hibaadatok. A `resource` mezot kompatibilitas miatt megtartjuk;
    // az uj kodban ugyanazt jelenti, mint a faultSource.
    // TODO: ezt megszuntetni!
    Resource_t* resource;

    // A hibat eredetileg okozo resource.
    Resource_t* faultSource;
    // A hibat a managernek jelento resource.
    Resource_t* reportedBy;
    // A hibas dependency, ha van ilyen.
    Resource_t* failedDependency;
    // A hiba eredetenek tipusa.
    ResourceErrorReason_t reason;
    // A recovery policy felulbiralasa.
    ResourceRecoveryOverride_t recoveryOverride;
} ResourceErrorInfo_t;
//------------------------------------------------------------------------------
// Resource allapotok.
typedef enum
{
    // Eroforras le van allitva
    RESOURCE_STOPPED = 0,
    // Eroforras indul
    RESOURCE_STARTING,
    // Eroforras mukodik
    RESOURCE_RUNNING,
    // Eroforras leallitas alatt van
    RESOURCE_STOPPING_,
    // Eroforras hibaallapotban van
    RESOURCE_ERROR_,
    // Az eroforras varakozik egy fuggosegere. (Csak ha minden fuggosege
    // elindult, akkor fog o maga is elindulni.)
    RESOURCE_WAITING_FOR_DEPENDENCY,
    // Az eroforras vegzett a feladataval. (Egyszer lefuto feladatok eseten)
    RESOURCE_DONE_
} ResourceState_t;
//------------------------------------------------------------------------------
// A user fele kuldott esemenyek.
typedef enum
{
    // Az eroforras elindult, hasznalhato
    RESOURCE_EVENT_STARTED = 0,
    // Az eroforras leallt
    RESOURCE_EVENT_STOPPED,
    // Az eroforras hibaallapotba kerult
    RESOURCE_EVENT_ERROR,
    // Az eroforrason hiba miatti helyreallitas kezdodott
    RESOURCE_EVENT_RECOVERY,
    // A hiba miatti helyreallitas sikeresen lefutott
    RESOURCE_EVENT_RECOVERED,
    // Az eroforras vegzett a feladataval. (Egyszer lefuto feladatok eseten)
    RESOURCE_EVENT_DONE
} ResourceEvent_t;
//------------------------------------------------------------------------------
// Dependency hiba kezelese.
typedef enum
{
    // A dependency hibaja figyelmen kivul marad.
    RESOURCE_DEP_IGNORE = 0,
    // A dependency hibaja a fuggo resource hibajat okozza.
    RESOURCE_DEP_PROPAGATE,
    // A dependency recoveryt kap.
    RESOURCE_DEP_AUTO_RECOVER
} ResourceDependencyErrorStrategy_t;
//------------------------------------------------------------------------------
// A kooperativ start/loop callbackek bemeneti es kimeneti vezerloadatai.
// Minden callback elott a manager/loop uj alapertelmezett ertekeket ad a
// waitTime es waitedEvents mezoknek; a callback ezeket a kovetkezo futasra
// allitja be. Az events az adott korben atvett alkalmazasi esemenymaszk.
// Nem illeszkedo esemeny nem futtat callbacket, es az atvetelkor elveszik.
typedef struct
{
    // Az adott callbackhoz atadott alkalmazasi esemenymaszk.
    uint64_t events;
    // A vart esemenyek maszkja. Csak az illeszkedo esemeny indit callbacket;
    // az INPUT jelzes csak a taskot ebreszti fel.
    uint64_t waitedEvents;
    // Igaz, ha a callbackot timer-lejarat valtja ki.
    bool timed;
    // A kovetkezo callbackig kert varakozas tickben.
    uint32_t waitTime;
    // Igaz, ha leallitasi kerelem van ervenyben.
    bool stopRequest;
    // Igaz, ha az eroforras befejezettnek jelenti magat.
    bool done;
    // Igaz, ha a callback megtiltja a leallast.
    bool prohibitStop;
    // A start callback ezzel jelzi a start befejezeset.
    bool run;
    // Igaz, ha a hiba utani lezaras befejezodott.
    bool errorClosed;
} ResourceControl_t;
//------------------------------------------------------------------------------
// Resource inditasi kerelem callback definialasa
// A startRequestFunc opcionalis, a manager taskjabol fut, es az inditasi
// igenyt tovabbitja. Nem kStatus_Success visszateres eseten a resource
// hibaallapotba kerul, a visszateresi status az errorInfo.status mezobe kerul.
// Loop nelkuli resource-nal, ha hianyzik, a Manager azonnal RESOURCE_RUNNING
// allapotot jelez. Loopos resource-nal a loop
// inditasi kerese ettol fuggetlenul tovabbitasra kerul.
// A tenyleges startFunc a ResourceLoop taskjaban fut, ha van loop.
typedef status_t (*Resource_startRequest_fn_t)(Resource_t* resource,
                                               void* callbackData);

// Resource leallitas kerelem callback definialasa
// A stopRequestFunc opcionalis, a manager taskjabol fut, es a leallitasi
// igenyt tovabbitja. Nem kStatus_Success visszateres eseten a resource
// hibaallapotba kerul, a visszateresi status az errorInfo.status mezobe kerul.
// Ha hianyzik, a Manager nem hiv callbacket. A resource sajat mukodese
// jelenti a leallast vagy a DONE allapotot.
// A tenyleges stopFunc a ResourceLoop taskjaban fut, ha van loop.
typedef status_t (*Resource_stopRequest_fn_t)(Resource_t* resource,
                                              void* callbackData);

// Loop nelkuli resource esemenycallbackjenek definialasa.
// A ResourceManager taskjabol fut.
typedef void (*Resource_event_fn_t)(Resource_t* resource,
                                    uint64_t events,
                                    void* callbackData);

// A kooperativ callbackek kizarolag a ResourceLoop taskjabol hivhatok.
// A start es loop callback a control mezokkel adhatja meg a kovetkezo
// esemenyszurot, idozitest, illetve a leallitas/befejezes allapotat.

// Taszkban futo cooperativ eroforras start callback definicioja.
typedef status_t Resource_start_fn_t(void* callbackData,
                                     ResourceControl_t* control);

// Taszkban futo cooperativ eroforras stop callback definicioja.
typedef status_t Resource_stop_fn_t(void* callbackData);

// Taszkban futo cooperativ eroforras loop callback definicioja.
typedef status_t Resource_loop_fn_t(void* callbackData,
                                    ResourceControl_t* control);

// Taszkban futo cooperativ eroforras error callback definicioja.
typedef void Resource_error_fn_t(void* callbackData, status_t errorCode);

// Taszkban futo cooperativ eroforras hiba kezelo ciklus inicializalo callback
// definicioja.
// (Hiba utani lezarasi allapotgep: az init callback egyszer fut, utana az
// errorLoopFunc ismetlodhet, amig a lezaro feltetelek teljesulnek.)
typedef status_t Resource_errorLoopStart_fn_t(void* callbackData,
                                              ResourceControl_t* control);

// Taszkban futo cooperativ eroforras hiba kezelo ciklus callback definicioja.
typedef status_t Resource_errorLoop_fn_t(void* callbackData,
                                         ResourceControl_t* control);

// User esemeny callback.
// A callback a manager mutex birtokaban fut.
// Emiatt a callbackbol biztonsagosan hivhato:
//   Resource_request()
//   Resource_release()
//   Resource_recover()
typedef void (*Resource_event_cb_t)(ResourceUser_t* user,
                                    ResourceEvent_t event,
                                    const ResourceErrorInfo_t* errorInfo,
                                    void* callbackData);

// Eroforras-szintu esemenyfigyelo callback.
// (Esemenyenkent egyszer fut le, fuggetlenul az aktiv userek szamatol.
typedef void (*Resource_observer_cb_t)(Resource_t* resource,
                                       ResourceEvent_t event,
                                       const ResourceErrorInfo_t* errorInfo,
                                       void* context);

// Manager-szintu callback, amely akkor hivodik meg, amikor minden regisztralt
// resource RESOURCE_STOPPED allapotu, nincs aktiv igenyles es a pending lista
// is ures. Indulaskor pusztan a kezdeti STOPPED allapot miatt nem fut; egy
// aktiv mukodesi ciklus leallasa utan egyszer hivodik meg, es uj ciklus utan
// ujra hivhato. A callback a manager mutexen kivul fut.
typedef void (*ResourceManager_all_stopped_cb_t)(ResourceManager_t* manager,
                                                 void* context);
//------------------------------------------------------------------------------
// Egy resource felhasznaloja. (Lancolt listaban tarolva. A lista init utan
// nem valtoztathato!)
struct _ResourceUser
{
    // A kovetkezo user a lancolt listaban.
    ResourceUser_t* next;

    // Diagnosztikai nev. (A hivo altal biztositott tartos string.)
    const char* name;

    // A user altal hasznalt eroforras.
    Resource_t* resource;

    // Igaz, ha a user aktiv igenyt tart fenn.
    bool requested;

    // Igaz, ha a user recoveryt kert.
    bool recoveryRequested;

    // Igaz, ha a user a sajat eroforras hibaja vagy propagalt dependency-hiba
    // Igaz, ha a user automatikus recoveryt ker hiba eseten.
    bool autoRecovery;

    // Igaz, ha a STARTED visszajelzest meg ki kell kuldeni, mert a user
    // mar RUNNING allapotban igenyelte az eroforrast.
    bool startedEventPending;

    // Igaz, ha a DONE esemenyt meg ki kell kuldeni, mert a user mar DONE
    // allapotban igenyelte az eroforrast.
    bool doneEventPending;

    // A user esemenyeire meghivando callback.
    Resource_event_cb_t eventFunc;
    // A callbacknak atadott alkalmazasi kontextus.
    void* callbackData;
};
//------------------------------------------------------------------------------
// Egy resource-hoz regisztralt esemenyfigyelo objektum.
// Az objektumot a hivo biztositja.
// A lista a manager indulasa utan nem modosithato!
struct _ResourceObserver
{
    // A kovetkezo observer a lancolt listaban.
    ResourceObserver_t* next;
    // Diagnosztikai nev. (A hivo altal biztositott tartos string.)
    const char* name;
    // A megfigyelt eroforras.
    Resource_t* resource;
    // Az eroforras esemenyeire hivando callback.
    Resource_observer_cb_t eventFunc;
    // A callbacknak atadott alkalmazasi kontextus.
    void* callbackData;
};
//------------------------------------------------------------------------------
// Resource dependency.
// (A dependency objektumot a hivo biztositja.)
struct _ResourceDependency
{
    // A kovetkezo dependency a dependency-listaban.
    ResourceDependency_t* next;

    // A kovetkezo dependency a dependensek lancolt listajaban.
    ResourceDependency_t* nextDependent;

    // A dependency-kent hasznalt eroforras.
    Resource_t* resource;

    // Az eroforras, amely ezt a dependency-t hasznalja.
    Resource_t* dependent;

    // A dependency-hiba kezelesenek modja.
    ResourceDependencyErrorStrategy_t errorStrategy;

    // Igaz, ha a PROPAGATE hibat a dependency gyors helyreallasa utan is
    // meg kell orizni a dependent feldolgozasaig.
    bool pendingError;
    // A megorzott dependency-hiba reszletes adatai.
    ResourceErrorInfo_t pendingErrorInfo;

    // A dependency eroforras igenylesere hasznalt belso user.
    // Ez a dependency resource refcountjaba ugyanugy beleszamit.
    ResourceUser_t internalUser;
};
//------------------------------------------------------------------------------
// Kooperativ eroforrasokhoz tartozo loop kezeles allapotai
typedef enum
{
    // Az eroforras az inditasra varakozik
    RESOURCE_LOOP_WAITING_FOR_START = 0,
    // Az eroforras indul
    RESOURCE_LOOP_STARTING,
    // Az eroforras elindult, mukodik
    RESOURCE_LOOP_RUNNING,
    // Az eroforras leall
    RESOURCE_LOOP_STOPPING,
    // Az eroforras hibaallapotban van
    RESOURCE_LOOP_ERROR,
    // Az eroforras a hiba utani lezarasi folyamatban van
    RESOURCE_LOOP_CLOSING_ERROR
} ResourceLoopState_t;
//------------------------------------------------------------------------------
// A ResourceLoop-hoz rendelt eroforras opcionalis kiegeszitoje.
// Csak loop alatt futo eroforras eseten kell statikusan peldanyositani.
struct ResourceLoopExtension
{
    // Az opcionalis kooperativ vegrehajtasi kornyezet.
    // Ha az eroforras loophoz van rendelve, ez nem NULL.
    ResourceLoop_t* loop;

    // A kovetkezo resource ugyanazon loop listajaban.
    Resource_t* loopNext;

    // A resource loophoz tartozo lifecycle allapota.
    ResourceLoopState_t loopState;

    // A callbackok kozotti vezerloadatok. (Loop hasznalat eseten)
    ResourceControl_t loopControl;

    // Task/ISR felol aszinkron gyujtott belso esemenyek. (Loop eseten)
    uint32_t loopControlEventsAsync;

    // A loop kovetkezo feldolgozasara varo belso esemenyek. (Loop eseten)
    uint32_t loopControlEvents;

    // A kovetkezo idozitett callback abszolut tickideje. (Loop eseten)
    uint64_t loopTimerDue;

    // Igaz, ha a loophoz tartozik aktiv idozites. (Loop eseten)
    bool loopTimerActive;

    // A loop allapotgep inicializalasa meg varat magara. (Loop eseten)
    bool loopStateInitPending;

    // Loop lifecycle generacio a regi muveletek kiszuresehez. (Loop eseten)
    uint32_t loopGeneration;

    // Igaz, ha a loop-hiba mar torolve lett. (Loop eseten)
    bool loopErrorCleared;

    // Igaz, ha leallitasi kerelem var feldolgozasra. (Loop eseten)
    bool loopStopPending;

    // A loop callback utolso hibakodja. (Loop eseten)
    status_t loopErrorCode;

    // A tenyleges inditasi callback.
    Resource_start_fn_t* startFunc;

    // A tenyleges leallitasi callback.
    Resource_stop_fn_t* stopFunc;

    // A kooperativ loop callback.
    Resource_loop_fn_t* loopFunc;

    // A hiba callbackje.
    Resource_error_fn_t* errorFunc;

    // Hiba utani lezaras inicializalasa.
    Resource_errorLoopStart_fn_t* errorLoopStartFunc;

    // Hiba utani lezarasi ciklus.
    Resource_errorLoop_fn_t* errorLoopFunc;
};
typedef struct ResourceLoopExtension ResourceLoopExtension_t;
//------------------------------------------------------------------------------
// A Resource objektum ritkan modositott allapotjelzoi.
// Ezeket a biteket a ResourceManager mutex vedelme alatt kell elerni.
typedef struct
{
    // Igaz, ha egy hibafeldolgozasi folyamat aktiv.
    bool faultProcessActive : 1;

    // Igaz, ha a recovery probalkozasi korlat sikertelenul elfogyott.
    bool recoveryFailed : 1;

    // Igaz, ha a resource a pending listaban szerepel.
    // A pending lista a manager kiszolgalasra varo eroforrasainak lancolt
    // listaja. A flag biztositja, hogy az eroforras csak egyszer szerepeljen.
    bool pending : 1;

    // Igaz, ha a STOPPING utan ujra kell inditani.
    bool restartAfterStop : 1;

    // Igaz, ha a leallast dependency-hiba okozta.
    // Ilyenkor a dependency helyreallasa utan sem indulhat ujra automatikusan.
    bool stoppedDueToDependencyError : 1;

    // Igaz, ha recovery-probalkozas folyamatban van.
    bool recoveryActive : 1;

    // Igaz, ha recovery inditasa varakozik.
    bool recoveryPending : 1;

    // Igaz, ha a recovery-szandeket dependency-k fele tovabb kell adni.
    bool recoveryCascadePending : 1;

    // Igaz, ha a resource lifecycle callbackjevel aktiv inditasi
    // kiserlet van folyamatban.
    bool lifecycleActive : 1;

    // Igaz, ha a stopRequest callback hibaval tert vissza. A callbacket
    // nem hivjuk ujra; a lifecycle STOPPED visszajelzessel zarhato le.
    bool stopRequestFailed : 1;

    // Igaz, ha a resource legalabb egyszer mar elerte a RUNNING allapotot.
    bool hasStarted : 1;

    // Igaz, ha az aktualis ERROR esemenyt mar elkuldte.
    bool errorEventSent : 1;

    // Igaz, ha az aktualis RUNNING allapothoz tartozo esemenyt mar elkuldte.
    bool runningEventSent : 1;

    // Igaz, ha az aktualis STOPPED esemenyt mar elkuldte.
    bool stoppedEventSent : 1;

    // Igaz, ha az aktualis DONE esemenyt mar elkuldte.
    bool doneEventSent : 1;
} ResourceFlags_t;
//------------------------------------------------------------------------------
// Resource (Eroforras) objektum.
struct _Resource
{
    // A kovetkezo eroforras a regisztralt resource-listaban.
    Resource_t* next;

    // A kovetkezo resource a kiszolgalasra varo listaban.
    Resource_t* pendingNext;

    // Diagnosztikai nev.  (Hivo altal biztositott tartos string.)
    const char* name;

    // A resource aktualis lifecycle-allapota.
    ResourceState_t state;

    // A resource-t kezelo ResourceManager objektum. (Ez ala van regisztralva)
    ResourceManager_t* manager;


    // Az aktualis lifecycle-hiba reszletes adatai.
    ResourceErrorInfo_t errorInfo;

    // Az utoljara feldolgozott hiba reszletes masolata.
    ResourceErrorInfo_t lastErrorInfo;
    // Az utoljara feldolgozott hiba status kodja.
    status_t lastErrorStatus;

    // A Resource ritkan modositott allapotjelzoi.
    ResourceFlags_t flags;

    // A resource alapertelmezett recovery-strategiaja.
    ResourceRecoveryPolicy_t recoveryPolicy;
    // A jelenlegi hibafolyamatra ervenyes felulbiralas.
    ResourceRecoveryOverride_t recoveryOverride;

    // A legutobbi inditas oka.
    ResourceStartReason_t startReason;
    // Az aktualis recovery oka.
    ResourceRecoveryReason_t recoveryReason;

    // Az aktiv igenylesek referencia-szama.
    uint32_t refCount;

    // A resource-hoz tartozo userek listajanak eleje.
    ResourceUser_t* users;
    // A resource-hoz tartozo userek listajanak vege.
    ResourceUser_t* usersLast;

    // A resource observer lista eleje.
    ResourceObserver_t* observers;
    // A resource observer lista vege.
    ResourceObserver_t* observersLast;

    // Az eroforras altal igenyelt dependency-k listaja.
    ResourceDependency_t* dependencies;

    // Az erre az eroforrasra hivatkozo dependency-k listaja.
    ResourceDependency_t* dependents;

    // Opcionalis callback az inditasi igeny tovabbitasahoz.
    // Loop nelkul, ha NULL, a Manager azonnal RESOURCE_RUNNING-et jelez.
    Resource_startRequest_fn_t startRequestFunc;
    // Opcionalis callback a leallitasi igeny tovabbitasahoz.
    // Ha NULL, a Manager nem hiv callbacket.
    Resource_stopRequest_fn_t stopRequestFunc;
    // Loop nelkuli resource esemenycallbackje.
    Resource_event_fn_t eventFunc;
    // A callbackekben atadott tetszoleges apllikacios adat (Contextus)
    void* callbackData;

    // Az opcionalis loop mukodes kiegeszitoje. NULL eseten nincs loop.
    ResourceLoopExtension_t* loopExtension;

    // Loop nelkuli esemenyek aszinkron tarolasara esemenybitmaszk.
    uint64_t inputEventsAsync;

    // A recovery-kaszkadot elindito hibaforras.
    // (Annak a hibaforrasnak a jelolese, amely recovery-kaszkadot kert.)
    Resource_t* recoveryFaultSource;

    // Az aktualis recovery-ciklus probalkozasainak szama.
    uint32_t recoveryAttempts;
    // A recovery-kiserletek maximuma; 0 = korlatlan.
    uint32_t recoveryMaxAttempts;

    // Generacio a regi aszinkron lifecycle-uzenetek kiszuresehez.
    // Uj STARTING, illetve terminalis STOPPED/DONE allapotnal valtozik.
    uint32_t lifecycleGeneration;

};
//------------------------------------------------------------------------------
// ResourceManager-adatok
struct _ResourceManager
{
    // A manager allapotat vedo rekurziv mutex handle-je.
    SemaphoreHandle_t mutex;
    // A mutexek taroloja a manager objektum resze
    StaticSemaphore_t mutexBuffer;

    // A manager task ebresztesere hasznalt szemafor handle-je.
    SemaphoreHandle_t wakeup;
    // Az ebreszto binaris szemafor statikus taroloja.
    StaticSemaphore_t wakeupBuffer;

    // A regisztralt resource-ok lancolt listajanak eleje.
    Resource_t* resources;
    // A regisztralt resource-ok lancolt listajanak utolso eleme.
    Resource_t* lastResource;

    // A kiszolgalasra varo resource-ok listajanak eleje.
    Resource_t* pendingResources;

    // Igaz az elso run() meghivasa utan.
    // Igaz, ha a manager futasa mar megkezdodott.
    bool running;

    // Igaz, ha a manager inicializalasa sikeres volt.
    bool initialized;

    // Callback minden resource leallasa utan.
    ResourceManager_all_stopped_cb_t allStoppedCallback;
    // Az allStoppedCallback kontextusa.
    void* allStoppedCallbackData;
    // Igaz, ha a rendszer a legutobbi osszes-leallt jelzes ota
    // legalabb egy eroforras nem-STOPPED allapotba lepett.
    // Ezzel elkeruljuk a callback meghivasat indulaskor, amikor minden
    // Igaz, ha egy aktiv ciklus utan jelezheto az osszes leallas.
    bool allStoppedCallbackArmed;
};
//------------------------------------------------------------------------------
// Eroforrasokhoz tartozo callbackek
typedef struct
{
    // Opcionalis callback az inditasi igeny tovabbitasahoz.
    // Loop nelkul, ha NULL, a Manager azonnal RESOURCE_RUNNING-et jelez.
    Resource_startRequest_fn_t startRequestFunc;
    // Opcionalis callback a leallitasi igeny tovabbitasahoz.
    // Ha NULL, a Manager nem hiv callbacket.
    Resource_stopRequest_fn_t stopRequestFunc;
    // A tenyleges inditasi callback loop eseten.
    Resource_start_fn_t* startFunc;
    // A tenyleges leallitasi callback loop eseten.
    Resource_stop_fn_t* stopFunc;
    // A periodikusan/esemenyre futo callback.
    Resource_loop_fn_t* loopFunc;
    // A hiba esemeny callbackje loop eseten.
    Resource_error_fn_t* errorFunc;
    // A hiba utani lezaras inicializalo callbackje loop eseten.
    Resource_errorLoopStart_fn_t* errorLoopStartFunc;
    // A hiba utani lezarasi ciklus callbackje loop eseten.
    Resource_errorLoop_fn_t* errorLoopFunc;
    // Opcionalis loop nelkuli esemenycallback, melyet akkor hiv, ha az
    // eroforras esemenyt kap.
    Resource_event_fn_t eventFunc;
} ResourceFuncs_t;
//------------------------------------------------------------------------------
// Eroforras letrehozasanal hasznalt konfiguracios struktura
typedef struct
{
    // Diagnosztikai nev; tartos stringre mutat.
    const char* name;
    // A resource lifecycle callbackjei.
    ResourceFuncs_t funcs;
    // A callbackek alkalmazasi kontextusa.
    void* callbackData;
    // Recovery probalkozasok maximuma; 0 = korlatlan.
    uint32_t recoveryMaxAttempts;
    // Loopos eroforras eseten a hivo altal statikusan biztositott kiegeszito.
    // Loop nelkuli eroforrasnal NULL.
    ResourceLoopExtension_t* loopExtension;
} ResourceConfig_t;
//------------------------------------------------------------------------------
// User letrehozasahoz hasznalt konfiguracios struktura
typedef  struct
{
    // Diagnosztikai nev; tartos stringre mutat.
    const char* name;
    // Az igenyelt eroforras.
    Resource_t* resource;
    // Az user esemenyeire hivando callback.
    Resource_event_cb_t eventFunc;
    // A callback alkalmazasi kontextusa.
    void* callbackData;
} ResourceUserConfig_t;
//------------------------------------------------------------------------------
// Eroforrasokhoz beregisztralhato ertesitesi objektum konfiguracioja
typedef struct
{
    // Diagnosztikai nev.  (Tartos stringre mutat.)
    const char* name;
    // A megfigyelt eroforras.
    Resource_t* resource;
    // Az eroforras esemenyeire hivando callback.
    Resource_observer_cb_t eventFunc;
    // A callback alkalmazasi kontextusa.
    void* callbackData;
} ResourceObserverConfig_t;
//------------------------------------------------------------------------------
// Manager inicializalasa.
// Inicializalja a managert, a mutexet es az ebreszto szemaforat.
bool ResourceManager_init(ResourceManager_t* manager);

// Resource inicializalasa.
// Inicialit egy resource objektumot a callbackekkel es recovery korlattal.
bool Resource_init(Resource_t* resource, const ResourceConfig_t* config);

// Resource hozzadasa a managerhez.
// (Csak init alatt hivhato.)
// A resource-t a managerhez rendeli. Csak init fazisban hivhato.
bool ResourceManager_addResource(ResourceManager_t* manager,
                                 Resource_t* resource);

// Eroforras leirojanak lekerdezese az eroforras neve alapjan.
// NULL-t ad vissza, ha az eroforras nem talalhato.
Resource_t* ResourceManager_getResourceByName(ResourceManager_t* manager,
                                              const char*  name);

// Egyszeri manager-szintu callback regisztralasa.
// Csak a ResourceManager_run() elinditasa elott hivhato. A callback akkor fut,
// ha minden resource RESOURCE_STOPPED allapotu, minden refCount nulla, a
// lifecycle callbackek nem aktivak es a pending lista ures. ERROR, DONE vagy
// WAITING_FOR_DEPENDENCY allapot eseten nem hivodik meg. Nem hivodik meg
// indulaskor pusztan azert, mert kezdetben minden resource STOPPED.
// A callback a manager mutexen kivul fut.
bool ResourceManager_registerAllStoppedCallback(ResourceManager_t* manager,
                                    ResourceManager_all_stopped_cb_t callback,
                                    void* callbackData);

// User inicializalasa es hozzadasa.
// User kapcsolatot hoz letre egy resource igenylesehez.
// (Csak init alatt hivhato.)
bool ResourceUser_init(ResourceUser_t* user,
                       const ResourceUserConfig_t* config);

// Resource esemenyfigyelo regisztralasa.
// KIZAROLAG az inicializalasi fazisban, a ResourceManager_run() inditasa
// elott hivhato! Nem hasznalja a manager mutexet!
bool ResourceObserver_init(ResourceObserver_t* observer,
                           const ResourceObserverConfig_t* config);

// Dependency hozzadasa.
// A dependency objektumot a hivo biztositja.
// A muvelet csak init alatt hivhato!
// A ciklusellenorzes alapertelmezesben ki van kapcsolva a sebesseg miatt.
// A konfiguraciot ciklusmentesen kell letrehozni. Ellenorzes bekapcsolasa:
// -DRESOURCE_MANAGER_ENABLE_DEPENDENCY_CYCLE_CHECK=1
bool Resource_addDependency(ResourceDependency_t* dependency,
                            Resource_t* resource,
                            Resource_t* dependencyResource,
                            ResourceDependencyErrorStrategy_t errorStrategy);

#ifndef MY_PASTER
#define MY_PASTER(a, b) a ## b
#endif
#ifndef MY_EVALUATOR
#define MY_EVALUATOR(a, b) MY_PASTER(a, b)
#endif
// Olyan specialis makro letrehozasa, mely segit definialni az eroforrasok kozti
// fuggosegeket. RESOURCE_DEP_PROPAGATE hibakezelessel
// A makro a forrassor alapjan egyedi, DEP_<sorszam> valtozot hoz letre.
// A letrehozott valtozo "static" elotagot kap, igy az nem a stacken jon letre,
// igy a definialodo fuggoseg leiro kesobb is a memoriaban marad.
// Ez a megoldas az alabbi kezi deklaraciot es inicializalast helyettesiti:
//   static resourceDep_t valami;
//   Resource_addDependency(&valami,
//                          &resource,
//                          &dependencyResource,
//                          RESOURCE_DEP_PROPAGATE);
#define CREATE_RESOURCE_DEPENDENCY_(resource, dependencyResource) \
{   static ResourceDependency_t MY_EVALUATOR(DEP_,__LINE__); \
    Resource_addDependency(& MY_EVALUATOR(DEP_,__LINE__), \
                           resource, \
                           dependencyResource, \
                           RESOURCE_DEP_PROPAGATE); \
}
// Ugyanez a makro, de a harmadik parameterben megadhato a
// hibakezelesi strategia.
#define CREATE_RESOURCE_DEPENDENCY__(resource, dependencyResource, \
                                   errorStrategy) \
{   static ResourceDependency_t MY_EVALUATOR(DEP_,__LINE__); \
    Resource_addDependency(& MY_EVALUATOR(DEP_,__LINE__), \
                           resource, \
                           dependencyResource, \
                           errorStrategy); \
}


// Userenkenti automatikus recovery beallitasa.
// Alapertelmezetten letiltva.
// Engedelyezeskor egy mar fennallo, sajat vagy propagalt dependency-hiba is
// recoveryt indithat. A beallitas futas kozben is modosithato.
// A mar kiadott recovery kerest a letiltas nem vonja vissza.
void ResourceUser_setAutoRecovery(ResourceUser_t* user, bool enabled);

// Resource igenylese.
// Nem inditja el azonnal a resource-t; a keres feldolgozasa a manager
// taskjaban tortenik.
void Resource_request(ResourceUser_t* user);

// Resource elengedese.
// Nem allitja le azonnal a resource-t; a keres feldolgozasa a manager
// taskjaban tortenik.
void Resource_release(ResourceUser_t* user);

// Fejlesztesi es konzolos tesztelesi API. Alapertelmezesben nincs leforditva;
// a RESOURCE_MANAGER_ENABLE_DEVELOPMENT_API=1 build kapcsoloval
// engedelyezheto.
// Nem kell hozza ResourceUser_t objektum. Minden start novel a refCount-on,
// ezert minden Resource_start() hivast egy Resource_stop() hivassal kell
// parositani. Eles alkalmazasi kodban ne engedelyezd! A lifecycle-t tovabbra
// is a manager task dolgozza fel aszinkron modon.
#if defined(RESOURCE_MANAGER_ENABLE_DEVELOPMENT_API) && \
    RESOURCE_MANAGER_ENABLE_DEVELOPMENT_API
void Resource_start(Resource_t* resource);
void Resource_stop(Resource_t* resource);
#endif

// Recovery igenylese.
// Nem inditja el azonnal a recoveryt; a keres feldolgozasa a manager
// taskjaban tortenik.
void Resource_recover(ResourceUser_t* user);

// Hibajelzes valamely eroforrasra.
// A reporter a faultSource erofrast jeloli meg a hiba forrasakent.
// A faultSource-nak a reporter dependenciafajaban kell lennie.
// A hibaforras es a hibastatusz az elso elfogadott jelzesbol marad meg.
// A recoveryOverride viszont az aktiv hibafolyamat alatt a
// FORCED > DISABLED > DEFAULT sorrendben erosodhet.
// Igaz ertekkel ter vissza, ha elfogadta a hiba jelzest.
bool Resource_reportError(Resource_t* reporter,
                          Resource_t* faultSource,
                          status_t status,
                          ResourceRecoveryOverride_t recoveryOverride);

// Csak a megadott eletciklushoz tartozo hibat fogadja el.
// (Resource_reportError-nal leirtak ervenyesek ra is)
bool Resource_reportErrorForGeneration(Resource_t* reporter,
                                       uint32_t generation,
                                       Resource_t* faultSource,
                                       status_t status,
                                   ResourceRecoveryOverride_t recoveryOverride);

// A hibaforras alapertelmezett recovery-strategiajanak beallitasa.
void Resource_setRecoveryPolicy(Resource_t* resource,
                                ResourceRecoveryPolicy_t policy);


// Az eroforras inditasi okanak lekerdezese
// Callbackbol is hivhato informacio lekerdezes.
ResourceStartReason_t Resource_getStartReason(Resource_t* resource);

// Az eroforras hiba okanak lekerdezese
// Callbackbol is hivhato informacio lekerdezes.
ResourceErrorReason_t Resource_getErrorReason(Resource_t* resource,
                                              ResourceErrorInfo_t* errorInfo);

// Az eroforras helyreallitasi okanak lekerdezese
// Callbackbol is hivhato informacio lekerdezes.
ResourceRecoveryReason_t Resource_getRecoveryReason(Resource_t* resource);

// A recovery sikeressegenek lekerdezese.
// Callbackbol is hivhato informacio lekerdezes.
// Igaz ertekkel ter vissza, ha a recovery-probalkozasi korlat sikertelenul
// elfogyott.
bool Resource_recoveryFailed(Resource_t* resource);

// Lifecycle-generacio lekerdezese.
// A start/stop callbackben, meg az aszinkron munka elinditasa elott kell
// lekerdezni es a workernek atadni.
// A STOPPED/DONE terminalis atmenet ervenyteleniti a lezart lifecycle
// generaciojat is.
uint32_t Resource_getLifecycleGeneration(Resource_t* resource);

// Generaciohoz kotott aszinkron allapotjelzes.
// A regi lifecycle-bol kesve beerkezo jelzeseket a manager eldobja.
// RESOURCE_ERROR eseten status nem lehet kStatus_Success.
// A tobbi allapotnal status legyen kStatus_Success.
void Resource_setStateForGeneration(Resource_t* resource,
                                    uint32_t generation,
                                    ResourceState_t state,
                                    status_t status);

// Legacy allapotjelzes. Szinkron callbackbol hasznalhato, vagy ha a hivo
// garantalja, hogy nincs regi lifecycle-bol kesve beerkezo statusz.
// Aszinkron workerbol a generacios API hasznalata javasolt.
void Resource_setState(Resource_t* resource,
                       ResourceState_t state,
                       status_t status);

// Eroforras-esemeny kuldese, task alol.
// ResourceLoop tag eseten a loop kapja meg az esemenyt, egyebkent az
//   opcionalis eventFunc a manager taskjabol fut le.
void Resource_setEvent(Resource_t* resource, uint64_t events);
// ISR-bol hivhato eroforras-esemeny kuldese.
// ResourceLoop tag eseten a loop kapja meg az esemenyt, egyebkent az
//   opcionalis eventFunc a manager taskjabol fut le.
void Resource_setEventFromIsr(Resource_t* resource, uint64_t events);

// Manager feldolgozo rutinja.
// Addig dolgozik, amig van pending resource, majd var a kovetkezo esemenyre.
// Feldolgozza a pending listat, majd var a kovetkezo ebresztesre.
void ResourceManager_run(ResourceManager_t* manager);

// Az osszes eroforras allapotanak es recovery-informacioinak kiirasa
// printf-fel.
// verbose=true eseten az osszes usert (a request flagjeikkel) es a fuggosegek
// listajat is kiirja.
// A mutex a konzisztens snapshot miatt a teljes konzolkiiras alatt zarva marad.
void ResourceManager_printStatus(ResourceManager_t* manager, bool verbose);

// Resource allapot lekerdezese.
ResourceState_t Resource_getState(Resource_t* resource);

// Resource refcount lekerdezese. (aktiv igenylesek referencia-szama)
uint32_t Resource_getRefCount(Resource_t* resource);

// User request allapot lekerdezese.
bool ResourceUser_isRequested(ResourceUser_t* user);

// User recovery igenyles lekerdezese.
bool ResourceUser_recoveryRequested(ResourceUser_t* user);

// Resource allapot stringesitese. Nyomkoveteshez
const char*  Resource_stateTosStr(ResourceState_t state);
// Resource esemeny stringesitese. Nyomkoveteshez
const char*  Resource_eventTosStr(ResourceEvent_t event);

#ifdef __cplusplus
}
#endif

//------------------------------------------------------------------------------
#endif
