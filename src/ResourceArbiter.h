#ifndef RESOURCE_ARBITER_H
#define RESOURCE_ARBITER_H

#include <stdbool.h>
#include <stdint.h>
#include "FreeRTOS.h"
#include "semphr.h"
//------------------------------------------------------------------------------
// A hozzaferes megszerzesekor meghivott callback definicioja
// A callback alatt egyetlen arbiter publikus API-ja sem hivhato meg.
// A callback mutex nelkul fut, de a dispatch mutexet a manager tartja.
typedef void (*ResourceArbiterGrantedFunc)(void *callbackData);
//------------------------------------------------------------------------------
// Az osszes hasznalati igeny megszunese utan meghivott callback
// Erre a callbackre is a fenti callback szerzodes ervenyes.
typedef void (*ResourceArbiterIdleFunc)(void *callbackData);
//------------------------------------------------------------------------------
// Keresek allapotai
typedef enum
{
    // Keres nem aktiv
    RESOURCE_ARBITER_REQUEST_IDLE = 0,
    // Varakozas, hogy a kerelmet befogadjak
    RESOURCE_ARBITER_REQUEST_WAITING,
    // A kerelm erevenyre jutott
    RESOURCE_ARBITER_REQUEST_GRANTED
} ResourceArbiterRequestState_t;
//------------------------------------------------------------------------------
// A ResourceArbiter tipus elore deklaralasa a request objektumhoz
typedef struct ResourceArbiter ResourceArbiter_t;
//------------------------------------------------------------------------------
// Prioritas szerint kezelt kerelem leiroja
typedef struct ResourceArbiterRequest
{
    // A kerelemhez tartozo arbiter
    ResourceArbiter_t *arbiter;

    // Kovetkezo kerelem a prioritasi varakozasi listaban
    struct ResourceArbiterRequest *next;

    // A callbacknak atadott felhasznaloi adat
    void *callbackData;

    // A hozzaferes megszerzesekor meghivando callback
    ResourceArbiterGrantedFunc grantedFunc;

    // A kerelem prioritasa, nagyobb ertek jelent nagyobb prioritast
    uint8_t priority;

    // A kerelem aktualis allapota
    ResourceArbiterRequestState_t state;
} ResourceArbiterRequest_t;
//------------------------------------------------------------------------------
// Prioritasokat kezelo manager valtozoi
struct ResourceArbiter
{
    // A belso allapot mutexenek statikus taroloja
    StaticSemaphore_t mutexBuffer;

    // A belso allapotot vedo mutex
    SemaphoreHandle_t mutex;

    // A callback mutexenek statikus taroloja
    StaticSemaphore_t dispatchMutexBuffer;

    // A callback futasat es az API muveleteket szerializalo mutex
    SemaphoreHandle_t dispatchMutex;

    // Prioritas szerint rendezett varakozasi lista
    ResourceArbiterRequest_t *waitingList;

    // Az eroforras aktualis birtokosa
    ResourceArbiterRequest_t *owner;

    // Az osszes igeny megszunese utan meghivando callback
    ResourceArbiterIdleFunc idleCallback;

    // Az idle callbacknak atadott felhasznaloi adat
    void *idleCallbackData;

    // Jelzi, hogy a manager inicializalasa sikeres volt-e
    bool initialized;
};
//------------------------------------------------------------------------------
// A manager mutexeinek letrehozasa es allapotanak inicializalasa
void ResourceArbiter_init(ResourceArbiter_t *arbiter);

// A manager mutexeinek felszabaditasa; csak ures manageren hivhato
void ResourceArbiter_deinit(ResourceArbiter_t *arbiter);

// Egy kerelmezo sajat kerelem objektumanak inicializalasa
void ResourceArbiterRequest_init(ResourceArbiterRequest_t *request,
                                 ResourceArbiter_t *arbiter,
                                 uint8_t priority,
                                 ResourceArbiterGrantedFunc grantedFunc,
                                 void *callbackData);

// Eroforras hasznalati igeny benyujtasa
void ResourceArbiter_request(ResourceArbiterRequest_t *request);

// Varakozo kerelem visszavonasa vagy a birtokolt eroforras visszaadasa
void ResourceArbiter_release(ResourceArbiterRequest_t *request);

// Az osszes aktiv es varakozo kerelem megszuntetese
void ResourceArbiter_clearRequests(ResourceArbiter_t *arbiter);

// Az idle callback regisztralasa; NULL callbackkel torolheto
void ResourceArbiter_setIdleCallback(ResourceArbiter_t *arbiter,
                                     ResourceArbiterIdleFunc idleCallback,
                                     void *callbackData);

// Varakozo vagy birtokos kerelem prioritasanak modositasa
void ResourceArbiter_setPriority(ResourceArbiterRequest_t *request,
                                 uint8_t priority);
//------------------------------------------------------------------------------
#endif
