#include <assert.h>
#include "ResourceArbiter.h"
//------------------------------------------------------------------------------
// A lista elso elemet birtokossa teszi, majd visszaadja a callback adatait
static void ResourceArbiter_grantNext(ResourceArbiter_t *arbiter,
                                  ResourceArbiterRequest_t **grantedRequest)
{
    // A segedfuggveny csak ures tulajdonosi allapotban hivhato.
    assert(arbiter != NULL);
    assert(grantedRequest != NULL);
    assert(arbiter->owner == NULL);

    // A lista elso eleme a legmagasabb prioritasu varakozo kerelem.
    *grantedRequest = arbiter->waitingList;
    if (*grantedRequest == NULL)
    {
        return;
    }

    // A kivalasztott kerelmet eltavolitjuk a varakozasi listabol.
    arbiter->waitingList = (*grantedRequest)->next;
    (*grantedRequest)->next = NULL;

    // A kerelem megkapja az eroforras hasznalati jogat.
    assert((*grantedRequest)->state == RESOURCE_ARBITER_REQUEST_WAITING);
    (*grantedRequest)->state = RESOURCE_ARBITER_REQUEST_GRANTED;
    arbiter->owner = *grantedRequest;
}
//------------------------------------------------------------------------------
// A manager mutexeinek letrehozasa es allapotanak inicializalasa
void ResourceArbiter_init(ResourceArbiter_t *arbiter)
{
    // Inicializalashoz ervenyes manager objektum szukseges.
    assert(arbiter != NULL);

    // A teljes kezdoallapot beallitasa a mutexek letrehozasa elott.
    arbiter->mutex = NULL;
    arbiter->dispatchMutex = NULL;
    arbiter->waitingList = NULL;
    arbiter->owner = NULL;
    arbiter->idleCallback = NULL;
    arbiter->idleCallbackData = NULL;
    arbiter->initialized = false;

    // A belso allapotot vedo mutex statikus taroloval keszul.
    arbiter->mutex = xSemaphoreCreateMutexStatic(
        &arbiter->mutexBuffer);
    assert(arbiter->mutex != NULL);

    // A dispatch mutex sorositsa az API muveleteket es callbackeket.
    arbiter->dispatchMutex = xSemaphoreCreateMutexStatic(
        &arbiter->dispatchMutexBuffer);
    assert(arbiter->dispatchMutex != NULL);

    // A manager csak mindket mutex letrehozasa utan hasznalhato.
    arbiter->initialized = true;
}
//------------------------------------------------------------------------------
// A manager mutexeinek felszabaditasa; csak ures manageren hivhato
void ResourceArbiter_deinit(ResourceArbiter_t *arbiter)
{
    // Csak ervenyesen inicializalt manager szuntetheto meg.
    assert(arbiter != NULL);
    assert(arbiter->initialized);

    // Az API muveletek es callbackek kizarasa utan ellenorizzuk az
    // eroforras allapotat.
    xSemaphoreTake(arbiter->dispatchMutex, portMAX_DELAY);
    xSemaphoreTake(arbiter->mutex, portMAX_DELAY);

    // A manager csak akkor szuntetheto meg, ha nincs aktiv kerelem.
    assert(arbiter->owner == NULL);
    assert(arbiter->waitingList == NULL);

    // A manager callback adatait is ervenytelenitjuk.
    arbiter->idleCallback = NULL;
    arbiter->idleCallbackData = NULL;

    // A tovabbi API-hivasok szamara ervenytelenitjuk a managert.
    arbiter->initialized = false;
    xSemaphoreGive(arbiter->mutex);
    xSemaphoreGive(arbiter->dispatchMutex);

    // Az ures manager mutexei mar biztonsagosan torolhetok.
    vSemaphoreDelete(arbiter->mutex);
    vSemaphoreDelete(arbiter->dispatchMutex);
    arbiter->mutex = NULL;
    arbiter->dispatchMutex = NULL;
}
//------------------------------------------------------------------------------
// Egy kerelmezo sajat kerelem objektumanak inicializalasa
void ResourceArbiterRequest_init(ResourceArbiterRequest_t *request,
                                 ResourceArbiter_t *arbiter,
                                 uint8_t priority,
                                 ResourceArbiterGrantedFunc grantedFunc,
                                 void *callbackData)
{
    // A kerelemhez ervenyes, mar inicializalt arbiter szukseges.
    assert(request != NULL);
    assert(arbiter != NULL);
    assert(arbiter->initialized);

    // Callback nelkul egy sikeres kiosztas nem lenne jelezheto.
    assert(grantedFunc != NULL);

    // A kerelem az arbiterhez kapcsolodik, de meg nem aktiv.
    request->arbiter = arbiter;
    request->next = NULL;
    request->callbackData = callbackData;
    request->grantedFunc = grantedFunc;
    request->priority = priority;
    request->state = RESOURCE_ARBITER_REQUEST_IDLE;
}
//------------------------------------------------------------------------------
// Eroforras hasznalati igeny benyujtasa
void ResourceArbiter_request(ResourceArbiterRequest_t *request)
{
    ResourceArbiter_t *arbiter;
    ResourceArbiterRequest_t **position;
    ResourceArbiterRequest_t *grantedRequest;

    // A kerelem es a hozza tartozo arbiter ellenorzese.
    assert(request != NULL);
    assert(request->arbiter != NULL);
    arbiter = request->arbiter;
    assert(arbiter->initialized);

    // A dispatch mutex megakadalyozza, hogy masik API-hivas vagy
    // callback ugyanazon a manageren parhuzamosan fusson.
    xSemaphoreTake(arbiter->dispatchMutex, portMAX_DELAY);

    // A belso mutex a tulajdonos es a varakozasi lista allapotat vedi.
    xSemaphoreTake(arbiter->mutex, portMAX_DELAY);

    // Egy kerelem egyszerre csak egy allapotban lehet jelen a managerben.
    assert(request->state == RESOURCE_ARBITER_REQUEST_IDLE);

    if (arbiter->owner == NULL)
    {
        // Szabad eroforras eseten a kerelmezo azonnal tulajdonossa valik.
        arbiter->owner = request;
        request->state = RESOURCE_ARBITER_REQUEST_GRANTED;
        grantedRequest = request;
    }
    else
    {
        // Foglalt eroforras eseten megkeressuk a beszurasi pontot.
        // A nagyobb prioritas elorebb kerul, az azonos prioritasu
        // kerelmek moge szurunk be a FIFO sorrend megtartasahoz.
        position = &arbiter->waitingList;
        while ((*position != NULL) &&
               ((*position)->priority >= request->priority))
        {
            position = &(*position)->next;
        }

        // A kerelmet a megtalalt helyre illesztjuk be.
        request->next = *position;
        request->state = RESOURCE_ARBITER_REQUEST_WAITING;
        *position = request;
        grantedRequest = NULL;
    }

    // A callback nem futhat a belso mutex birtokaban, mert sajat
    // feldolgozasa tovabbi manager muveleteket is igenyelhet.
    xSemaphoreGive(arbiter->mutex);

    if (grantedRequest != NULL)
    {
        // A callback jelzi a usernek az azonnali jogosultsagszerzest.
        // A dispatch mutex kozben tovabbra is szerializalja a muveleteket.
        grantedRequest->grantedFunc(grantedRequest->callbackData);
    }

    // A callback vegevel a kovetkezo manager muvelet is futhat.
    xSemaphoreGive(arbiter->dispatchMutex);
}
//------------------------------------------------------------------------------
// Varakozo kerelem visszavonasa vagy a birtokolt eroforras visszaadasa
void ResourceArbiter_release(ResourceArbiterRequest_t *request)
{
    ResourceArbiter_t *arbiter;
    ResourceArbiterRequest_t *grantedRequest;
    ResourceArbiterRequest_t **position;

    // A kerelem es a hozza tartozo arbiter ellenorzese.
    assert(request != NULL);
    assert(request->arbiter != NULL);
    arbiter = request->arbiter;
    assert(arbiter->initialized);

    // A felszabaditast is sorositjuk a tobbi API-hivassal es callbackkel.
    xSemaphoreTake(arbiter->dispatchMutex, portMAX_DELAY);
    xSemaphoreTake(arbiter->mutex, portMAX_DELAY);

    if (request->state == RESOURCE_ARBITER_REQUEST_IDLE)
    {
        // Az inaktiv kerelem nincs a manager listajaban, nincs mit torolni.
        xSemaphoreGive(arbiter->mutex);
        xSemaphoreGive(arbiter->dispatchMutex);
        return;
    }

    if (request->state == RESOURCE_ARBITER_REQUEST_WAITING)
    {
        // A varakozo kerelmet megkeressuk a prioritas szerint rendezett
        // lancolt listaban.
        position = &arbiter->waitingList;
        while ((*position != NULL) && (*position != request))
        {
            position = &(*position)->next;
        }

        // A WAITING allapotu kerelemnek szerepelnie kell a listaban.
        assert(*position == request);

        if (*position == request)
        {
            // A kerelmet eltavolitjuk a listabol, es ujra felhasznalhatova
            // tesszuk. A jelenlegi tulajdonos jogat ez nem erinti.
            *position = request->next;
            request->next = NULL;
            request->state = RESOURCE_ARBITER_REQUEST_IDLE;
        }

        xSemaphoreGive(arbiter->mutex);
        xSemaphoreGive(arbiter->dispatchMutex);
        return;
    }

    if (request->state != RESOURCE_ARBITER_REQUEST_GRANTED)
    {
        // Ismeretlen allapot eseten a manager belso allapotat nem modositjuk.
        assert(false);
        xSemaphoreGive(arbiter->mutex);
        xSemaphoreGive(arbiter->dispatchMutex);
        return;
    }

    // A GRANTED allapotu kerelem csak akkor adhatja vissza a jogot,
    // ha tenylegesen o az aktualis tulajdonos.
    if (arbiter->owner != request)
    {
        xSemaphoreGive(arbiter->mutex);
        xSemaphoreGive(arbiter->dispatchMutex);
        return;
    }

    // A regi birtokos kerelme ujra felhasznalhato allapotba kerul.
    request->state = RESOURCE_ARBITER_REQUEST_IDLE;
    arbiter->owner = NULL;

    // Ha van varakozo, a rendezett lista elso eleme kapja meg a jogot.
    ResourceArbiter_grantNext(arbiter, &grantedRequest);

    // A callback futasahoz elengedjuk a belso mutexet.
    xSemaphoreGive(arbiter->mutex);

    if (grantedRequest != NULL)
    {
        // Az uj tulajdonos ertesitese meg a kovetkezo API-muvelet elott.
        grantedRequest->grantedFunc(grantedRequest->callbackData);
    }
    else if (arbiter->idleCallback != NULL)
    {
        // Varakozo hianyaban az eroforras hasznalat nelkul marad.
        arbiter->idleCallback(arbiter->idleCallbackData);
    }

    // A kiosztas es az ertesites utan a manager ujra elerheto.
    xSemaphoreGive(arbiter->dispatchMutex);
}
//------------------------------------------------------------------------------
// Az osszes aktiv es varakozo kerelem megszuntetese
void ResourceArbiter_clearRequests(ResourceArbiter_t *arbiter)
{
    ResourceArbiterRequest_t *request;
    ResourceArbiterRequest_t *nextRequest;
    bool hadRequests;

    // Csak ervenyesen inicializalt manager kerelmei torolhetok.
    assert(arbiter != NULL);
    assert(arbiter->initialized);

    // A teljes torlest mas API-muvelettel es callbackkel szemben sorositjuk.
    xSemaphoreTake(arbiter->dispatchMutex, portMAX_DELAY);
    xSemaphoreTake(arbiter->mutex, portMAX_DELAY);

    // Eltaroljuk, hogy a muvelet elott volt-e barmilyen aktiv igeny.
    hadRequests = (arbiter->owner != NULL) ||
                  (arbiter->waitingList != NULL);

    // Minden varakozo kerelmet levalasztunk es ujra felhasznalhatova teszunk.
    request = arbiter->waitingList;
    arbiter->waitingList = NULL;
    while (request != NULL)
    {
        nextRequest = request->next;
        assert(request->state == RESOURCE_ARBITER_REQUEST_WAITING);
        request->next = NULL;
        request->state = RESOURCE_ARBITER_REQUEST_IDLE;
        request = nextRequest;
    }

    // Az aktualis birtokos is elvesziti a hasznalati jogot.
    if (arbiter->owner != NULL)
    {
        assert(arbiter->owner->state == RESOURCE_ARBITER_REQUEST_GRANTED);
        arbiter->owner->next = NULL;
        arbiter->owner->state = RESOURCE_ARBITER_REQUEST_IDLE;
        arbiter->owner = NULL;
    }

    // A callback mar az ures allapotot latja, de a tobbi API meg var.
    xSemaphoreGive(arbiter->mutex);

    if (hadRequests && (arbiter->idleCallback != NULL))
    {
        // Az idle callback egyszer fut le az osszes kerelem torlese utan.
        arbiter->idleCallback(arbiter->idleCallbackData);
    }

    // A teljes torles es az ertesites utan a manager ujra elerheto.
    xSemaphoreGive(arbiter->dispatchMutex);
}
//------------------------------------------------------------------------------
// Az idle callback regisztralasa vagy torlese
void ResourceArbiter_setIdleCallback(ResourceArbiter_t *arbiter,
                                     ResourceArbiterIdleFunc idleCallback,
                                     void *callbackData)
{
    // Csak ervenyesen inicializalt manager callbackje allithato.
    assert(arbiter != NULL);
    assert(arbiter->initialized);

    // A callback regisztraciojat a tobbi API-muvelettel sorositjuk.
    xSemaphoreTake(arbiter->dispatchMutex, portMAX_DELAY);
    xSemaphoreTake(arbiter->mutex, portMAX_DELAY);

    // NULL callback eseten az ertesites kikapcsolhato.
    arbiter->idleCallback = idleCallback;
    arbiter->idleCallbackData = callbackData;

    xSemaphoreGive(arbiter->mutex);
    xSemaphoreGive(arbiter->dispatchMutex);
}
//------------------------------------------------------------------------------
// Egy aktiv kerelem prioritasanak modositasa
void ResourceArbiter_setPriority(ResourceArbiterRequest_t *request,
                                 uint8_t priority)
{
    ResourceArbiter_t *arbiter;
    ResourceArbiterRequest_t **position;
    ResourceArbiterRequest_t **removePosition;

    // A kerelem es a hozza tartozo arbiter ellenorzese.
    assert(request != NULL);
    assert(request->arbiter != NULL);
    arbiter = request->arbiter;
    assert(arbiter->initialized);

    // A prioritasi sorrend modositasat mas manager muvelettel
    // es callbackkel szemben is sorositjuk.
    xSemaphoreTake(arbiter->dispatchMutex, portMAX_DELAY);
    xSemaphoreTake(arbiter->mutex, portMAX_DELAY);

    if (request->state == RESOURCE_ARBITER_REQUEST_WAITING)
    {
        // A varakozo kerelmet eloszor megkeressuk a lancolt listaban.
        removePosition = &arbiter->waitingList;
        while ((*removePosition != NULL) &&
               (*removePosition != request))
        {
            removePosition = &(*removePosition)->next;
        }

        // A varakozo allapotnak megfelelo kerelemnek a listaban kell lennie.
        assert(*removePosition == request);

        if (*removePosition == request)
        {
            // Eltavolitjuk a regi helyerol, majd az uj prioritasaval
            // keressuk meg a megfelelo beszurasi pontot.
            *removePosition = request->next;
            request->priority = priority;
            position = &arbiter->waitingList;

            // Az azonos prioritasu kerelmek moge kerul, igy a beszuras
            // pillanatatol uj FIFO sorrend ervenyesul.
            while ((*position != NULL) &&
                   ((*position)->priority >= request->priority))
            {
                position = &(*position)->next;
            }

            // Az uj helyre illesztjuk a varakozo kerelmet.
            request->next = *position;
            *position = request;
        }
    }
    else if (request->state == RESOURCE_ARBITER_REQUEST_GRANTED)
    {
        // A birtokos prioritasa is modosithato, de ettol a jogot
        // nem vesszuk el tole, vagyis nincs preempcio.
        assert(arbiter->owner == request);
        if (arbiter->owner == request)
        {
            request->priority = priority;
        }
    }
    else
    {
        // Csak varakozo vagy birtokos kerelem prioritasa modosithato.
        assert(false);
    }

    // A modositott allapotot mindket mutex elengedese utan latjak
    // a kovetkezo manager muveletek.
    xSemaphoreGive(arbiter->mutex);
    xSemaphoreGive(arbiter->dispatchMutex);
}
//------------------------------------------------------------------------------
