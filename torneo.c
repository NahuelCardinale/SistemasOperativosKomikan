#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define LADO 5
#define NODOS 28
#define T_TOP 25
#define T_IZQ 26
#define T_DER 27
#define PERROS 12
#define PIEZAS 13
#define PUMA 0
#define VACIO -1

typedef struct {
    unsigned semilla;
    int delay_min, delay_max;
    int puma;
    int perros[PERROS];
    int nperros;
    int max_movs;
    int verbose;
    int prob_fallo;
} Config;

typedef struct {
    int ocupante[NODOS];
    int pos[PIEZAS];
    bool vivo[PIEZAS];
    int capturas;
} Tablero;

typedef struct Mesa Mesa;

typedef struct {
    Mesa *mesa;
    int id;
} ArgPieza;

struct Mesa {
    int id;
    pthread_t juez;
    pthread_cond_t cv_asignada;
    bool asignada, en_juego;
    int jug_puma, jug_perros;

    pthread_mutex_t mtx;
    pthread_cond_t cv;
    Tablero tab;
    bool terminada;
    int ganador;
    const char *motivo;
    int movs, rechazos, colisiones;
    int pieza_falla, falla_en;
    pthread_t piezas[PIEZAS];
    ArgPieza args[PIEZAS];
    int partidas;
};

typedef struct {
    int id;
    pthread_t hilo;
    pthread_cond_t cv;
    bool en_cola;
    int mesa;
    const char *resultado;
} Jugador;

static Config cfg;
static int N, M;
static int vecinos[NODOS][4], nvec[NODOS], salto[NODOS][NODOS], gx[NODOS], gy[NODOS];

static pthread_mutex_t srv_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv_mm = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t log_mtx = PTHREAD_MUTEX_INITIALIZER;
static int *cola, cola_ini, cola_len;
static int llegados, jugadores_listos, mesas_libres, activas, max_activas;
static int partidas_jugadas, gana_puma, gana_perros, empates, abandonos, interrumpidas;
static bool cierre, mm_fin;
static Jugador *jugadores;
static Mesa *mesas;
static pthread_t matchmaker;

static const char *NOMBRE_RES[] = {"ACEPTADO", "ACEPTADO (CAPTURA)", "RECHAZADO: pieza capturada",
                                   "RECHAZADO: la pieza ya no esta en el origen", "RECHAZADO: no adyacente",
                                   "RECHAZADO: casilla ocupada (COLISION)", "RECHAZADO: partida terminada"};
enum { OK, CAPTURA, R_CAPTURADA, R_ORIGEN, R_NO_ADYACENTE, R_OCUPADA, R_TERMINADA };

static void log_msg(const char *fmt, ...) {
    va_list ap;
    pthread_mutex_lock(&log_mtx);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
    pthread_mutex_unlock(&log_mtx);
}

static int nodo(int x, int y) { return y * LADO + x; }

static void nombre_nodo(int n, char *buf) {
    if (n == T_TOP) strcpy(buf, "TRONO_TOP");
    else if (n == T_IZQ) strcpy(buf, "TRONO_IZQ");
    else if (n == T_DER) strcpy(buf, "TRONO_DER");
    else sprintf(buf, "(%d,%d)", n % LADO, n / LADO);
}

static void unir(int a, int b) {
    vecinos[a][nvec[a]++] = b;
    vecinos[b][nvec[b]++] = a;
}

static bool adyacentes(int a, int b) {
    for (int i = 0; i < nvec[a]; i++)
        if (vecinos[a][i] == b) return true;
    return false;
}

static void crear_tablero(void) {
    for (int y = 0; y < LADO; y++)
        for (int x = 0; x < LADO; x++) {
            gx[nodo(x, y)] = 2 * x;
            gy[nodo(x, y)] = 2 * y;
            if (x + 1 < LADO) unir(nodo(x, y), nodo(x + 1, y));
            if (y + 1 < LADO) unir(nodo(x, y), nodo(x, y + 1));
        }
    gx[T_IZQ] = 2; gy[T_IZQ] = 10;
    gx[T_DER] = 6; gy[T_DER] = 10;
    gx[T_TOP] = 4; gy[T_TOP] = 12;
    unir(T_TOP, T_IZQ);
    unir(T_TOP, T_DER);
    unir(T_IZQ, T_DER);
    unir(T_TOP, nodo(2, 4));
    unir(T_IZQ, nodo(0, 4));
    unir(T_IZQ, nodo(1, 4));
    unir(T_DER, nodo(3, 4));
    unir(T_DER, nodo(4, 4));
    for (int a = 0; a < NODOS; a++)
        for (int b = 0; b < NODOS; b++) salto[a][b] = VACIO;
    for (int a = 0; a < NODOS; a++)
        for (int i = 0; i < nvec[a]; i++) {
            int b = vecinos[a][i];
            for (int c = 0; c < NODOS; c++)
                if (gx[c] == 2 * gx[b] - gx[a] && gy[c] == 2 * gy[b] - gy[a] && adyacentes(b, c)) salto[a][b] = c;
        }
}

static int validar(const Tablero *t, int p, int desde, int hasta, int *cap) {
    *cap = VACIO;
    if (!t->vivo[p]) return R_CAPTURADA;
    if (t->pos[p] != desde) return R_ORIGEN;
    if (hasta < 0 || hasta >= NODOS || desde == hasta) return R_NO_ADYACENTE;
    if (adyacentes(desde, hasta)) return t->ocupante[hasta] == VACIO ? OK : R_OCUPADA;
    if (p == PUMA)
        for (int i = 0; i < nvec[desde]; i++) {
            int sobre = vecinos[desde][i];
            if (salto[desde][sobre] == hasta && t->ocupante[sobre] > PUMA) {
                if (t->ocupante[hasta] != VACIO) return R_OCUPADA;
                *cap = t->ocupante[sobre];
                return CAPTURA;
            }
        }
    return R_NO_ADYACENTE;
}

static void aplicar(Tablero *t, int p, int desde, int hasta, int cap) {
    t->ocupante[desde] = VACIO;
    t->ocupante[hasta] = p;
    t->pos[p] = hasta;
    if (cap != VACIO) {
        t->ocupante[t->pos[cap]] = VACIO;
        t->pos[cap] = VACIO;
        t->vivo[cap] = false;
        t->capturas++;
    }
}

static int movimientos(const Tablero *t, int p, int dest[], int caps[]) {
    int n = 0, d = t->pos[p];
    for (int i = 0; i < nvec[d]; i++) {
        int v = vecinos[d][i];
        if (t->ocupante[v] == VACIO) {
            dest[n] = v;
            caps[n++] = VACIO;
        } else if (p == PUMA && salto[d][v] != VACIO && t->ocupante[salto[d][v]] == VACIO) {
            dest[n] = salto[d][v];
            caps[n++] = t->ocupante[v];
        }
    }
    return n;
}

static bool puma_acorralado(const Tablero *t) {
    int d[8], c[8];
    return movimientos(t, PUMA, d, c) == 0;
}

static bool amenazada(const Tablero *t, int casilla, int perro) {
    int p = t->pos[PUMA];
    if (!adyacentes(p, casilla) || salto[p][casilla] == VACIO) return false;
    int destino = t->ocupante[salto[p][casilla]];
    return destino == VACIO || destino == perro;
}

static int distancia(int a, int b) { return abs(gx[a] - gx[b]) + abs(gy[a] - gy[b]); }

static int decidir(const Tablero *t, int p, unsigned *semilla) {
    int dest[8], caps[8];
    int n = movimientos(t, p, dest, caps);
    if (n == 0) return VACIO;
    if (p == PUMA) {
        for (int i = 0; i < n; i++)
            if (caps[i] != VACIO) return dest[i];
        return dest[rand_r(semilla) % n];
    }
    int aqui = t->pos[p], puma = t->pos[PUMA];
    bool en_peligro = amenazada(t, aqui, p);
    if (!en_peligro && adyacentes(aqui, puma) && rand_r(semilla) % 100 < 80) return VACIO;
    int mejor = VACIO, mejor_puntaje = 1000000;
    for (int i = 0; i < n; i++) {
        int puntaje = distancia(dest[i], puma) * 10 + rand_r(semilla) % 5;
        if (amenazada(t, dest[i], p)) puntaje += 1000;
        if (puntaje < mejor_puntaje) {
            mejor_puntaje = puntaje;
            mejor = dest[i];
        }
    }
    if (mejor_puntaje >= 1000 && !en_peligro) return VACIO;
    return mejor;
}

static void imprimir_tablero(Mesa *m) {
    char buf[1024], c[NODOS][4];
    for (int n = 0; n < NODOS; n++) {
        int p = m->tab.ocupante[n];
        if (p == VACIO) strcpy(c[n], " . ");
        else if (p == PUMA) strcpy(c[n], " P ");
        else sprintf(c[n], "D%-2d", (p - 1) % 100);
    }
    int k = sprintf(buf, "[Mesa %d] Tablero (capturas=%d):\n            [%s]\n      [%s]-------[%s]\n", m->id,
                    m->tab.capturas, c[T_TOP], c[T_IZQ], c[T_DER]);
    for (int y = LADO - 1; y >= 0; y--) {
        k += sprintf(buf + k, "  y=%d ", y);
        for (int x = 0; x < LADO; x++) k += sprintf(buf + k, "[%s]", c[nodo(x, y)]);
        k += sprintf(buf + k, "\n");
    }
    sprintf(buf + k, "        x=0  x=1  x=2  x=3  x=4");
    log_msg("%s", buf);
}

static void terminar(Mesa *m, int ganador, const char *motivo) {
    if (m->terminada) return;
    m->terminada = true;
    m->ganador = ganador;
    m->motivo = motivo;
    pthread_cond_broadcast(&m->cv);
}

static void revisar_dueno_muerto(Mesa *m, int rc) {
    if (rc != EOWNERDEAD) return;
    pthread_mutex_consistent(&m->mtx);
    log_msg("[Mesa %d] JUEZ: un hilo murio con el mutex tomado, mutex recuperado (EOWNERDEAD)", m->id);
    terminar(m, m->pieza_falla == PUMA ? 2 : 1, "abandono por fallo interno del rival");
}

static void lock_mesa(Mesa *m) { revisar_dueno_muerto(m, pthread_mutex_lock(&m->mtx)); }

static void mover(Mesa *m, int p, int desde, int hasta) {
    char a[16], b[16], quien[24];
    int cap;
    int r = m->terminada ? R_TERMINADA : validar(&m->tab, p, desde, hasta, &cap);
    nombre_nodo(desde, a);
    nombre_nodo(hasta, b);
    if (p == PUMA) strcpy(quien, "PUMA");
    else sprintf(quien, "PERRO %d", p - 1);
    if (r != OK && r != CAPTURA) {
        m->rechazos++;
        if (r == R_OCUPADA) m->colisiones++;
        log_msg("[Mesa %d] %s %s -> %s: %s", m->id, quien, a, b, NOMBRE_RES[r]);
        return;
    }
    aplicar(&m->tab, p, desde, hasta, cap);
    m->movs++;
    if (cap != VACIO) log_msg("[Mesa %d] CAPTURA: PUMA %s -> %s salta sobre PERRO %d", m->id, a, b, cap - 1);
    else log_msg("[Mesa %d] %s %s -> %s: %s", m->id, quien, a, b, NOMBRE_RES[r]);
    if (cfg.verbose) imprimir_tablero(m);
    if (m->tab.capturas >= 6) terminar(m, 1, "el Puma capturo 6 perros");
    else if (puma_acorralado(&m->tab)) terminar(m, 2, "el Puma quedo acorralado");
    else if (cfg.max_movs > 0 && m->movs >= cfg.max_movs) terminar(m, 3, "empate por limite de movimientos");
}

static void plazo_ms(struct timespec *ts, int ms) {
    clock_gettime(CLOCK_REALTIME, ts);
    ts->tv_sec += ms / 1000;
    ts->tv_nsec += (ms % 1000) * 1000000L;
    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec++;
        ts->tv_nsec -= 1000000000L;
    }
}

static void *hilo_pieza(void *arg) {
    ArgPieza *a = arg;
    Mesa *m = a->mesa;
    int id = a->id;
    unsigned semilla = cfg.semilla * 31u + (unsigned)(m->id * 1000 + m->partidas * 100 + id);
    lock_mesa(m);
    while (!m->terminada && m->tab.vivo[id]) {
        struct timespec ts;
        plazo_ms(&ts, cfg.delay_min + (int)(rand_r(&semilla) % (unsigned)(cfg.delay_max - cfg.delay_min + 1)));
        while (!m->terminada && m->tab.vivo[id]) {
            int rc = pthread_cond_timedwait(&m->cv, &m->mtx, &ts);
            revisar_dueno_muerto(m, rc);
            if (rc == ETIMEDOUT) break;
        }
        if (m->terminada || !m->tab.vivo[id]) break;
        if (m->pieza_falla == id && m->movs >= m->falla_en) {
            log_msg("[Mesa %d] FALLO: el hilo de la pieza %d termina con el mutex tomado", m->id, id);
            pthread_exit(NULL);
        }
        Tablero copia = m->tab;
        pthread_mutex_unlock(&m->mtx);
        int destino = decidir(&copia, id, &semilla);
        lock_mesa(m);
        if (destino != VACIO && !m->terminada && m->tab.vivo[id]) mover(m, id, copia.pos[id], destino);
    }
    pthread_mutex_unlock(&m->mtx);
    return NULL;
}

static void jugar_partida(Mesa *m) {
    pthread_mutexattr_t at;
    pthread_mutexattr_init(&at);
    pthread_mutexattr_setrobust(&at, PTHREAD_MUTEX_ROBUST);
    pthread_mutex_init(&m->mtx, &at);
    pthread_mutexattr_destroy(&at);
    pthread_cond_init(&m->cv, NULL);

    for (int n = 0; n < NODOS; n++) m->tab.ocupante[n] = VACIO;
    for (int p = 0; p < PIEZAS; p++) {
        int n = p == PUMA ? cfg.puma : cfg.perros[p - 1];
        m->tab.pos[p] = n;
        m->tab.ocupante[n] = p;
        m->tab.vivo[p] = true;
    }
    m->tab.capturas = 0;
    m->terminada = false;
    m->ganador = 0;
    m->motivo = "";
    m->movs = m->rechazos = m->colisiones = 0;
    unsigned semilla = cfg.semilla + (unsigned)(m->id * 7 + m->partidas * 13);
    m->pieza_falla = VACIO;
    if ((int)(rand_r(&semilla) % 100) < cfg.prob_fallo) {
        m->pieza_falla = (int)(rand_r(&semilla) % PIEZAS);
        m->falla_en = 5 + (int)(rand_r(&semilla) % 30);
    }

    pthread_mutex_lock(&srv_mtx);
    m->en_juego = true;
    bool cerrado = cierre;
    pthread_mutex_unlock(&srv_mtx);

    log_msg("[Mesa %d] JUEZ: inicia partida. Puma = jugador %d, Perros = jugador %d", m->id, m->jug_puma,
            m->jug_perros);
    lock_mesa(m);
    if (cfg.verbose) imprimir_tablero(m);
    if (cerrado) terminar(m, 0, "servidor interrumpido (SIGINT)");
    else if (puma_acorralado(&m->tab)) terminar(m, 2, "el Puma quedo acorralado");
    pthread_mutex_unlock(&m->mtx);

    int creadas = 0;
    for (int i = 0; i < PIEZAS; i++) {
        m->args[i].mesa = m;
        m->args[i].id = i;
        if (pthread_create(&m->piezas[i], NULL, hilo_pieza, &m->args[i]) != 0) {
            lock_mesa(m);
            terminar(m, 0, "error al crear hilos");
            pthread_mutex_unlock(&m->mtx);
            break;
        }
        creadas++;
    }

    lock_mesa(m);
    while (!m->terminada) revisar_dueno_muerto(m, pthread_cond_wait(&m->cv, &m->mtx));
    pthread_mutex_unlock(&m->mtx);
    for (int i = 0; i < creadas; i++) pthread_join(m->piezas[i], NULL);

    const char *quien[] = {"NADIE", "PUMA", "PERROS", "EMPATE"};
    log_msg("[Mesa %d] FIN: gana %s (%s). Movimientos=%d rechazados=%d colisiones=%d capturas=%d", m->id,
            quien[m->ganador], m->motivo, m->movs, m->rechazos, m->colisiones, m->tab.capturas);
    if (cfg.verbose) imprimir_tablero(m);

    pthread_mutex_lock(&srv_mtx);
    m->en_juego = false;
    pthread_mutex_unlock(&srv_mtx);
    pthread_cond_destroy(&m->cv);
    pthread_mutex_destroy(&m->mtx);
}

static void *hilo_juez(void *arg) {
    Mesa *m = arg;
    for (;;) {
        pthread_mutex_lock(&srv_mtx);
        while (!m->asignada && !mm_fin) pthread_cond_wait(&m->cv_asignada, &srv_mtx);
        if (!m->asignada) {
            pthread_mutex_unlock(&srv_mtx);
            return NULL;
        }
        activas++;
        if (activas > max_activas) max_activas = activas;
        pthread_mutex_unlock(&srv_mtx);

        jugar_partida(m);

        pthread_mutex_lock(&srv_mtx);
        Jugador *jp = &jugadores[m->jug_puma], *jd = &jugadores[m->jug_perros];
        bool abandono = strncmp(m->motivo, "abandono", 8) == 0;
        if (m->ganador == 1) {
            jp->resultado = abandono ? "GANO por abandono" : "GANO";
            jd->resultado = abandono ? "PERDIO por abandono" : "PERDIO";
        } else if (m->ganador == 2) {
            jp->resultado = abandono ? "PERDIO por abandono" : "PERDIO";
            jd->resultado = abandono ? "GANO por abandono" : "GANO";
        } else if (m->ganador == 3) {
            jp->resultado = jd->resultado = "EMPATE";
        } else {
            jp->resultado = jd->resultado = "partida interrumpida";
        }
        if (abandono) abandonos++;
        else if (m->ganador == 1) gana_puma++;
        else if (m->ganador == 2) gana_perros++;
        else if (m->ganador == 3) empates++;
        else interrumpidas++;
        partidas_jugadas++;
        m->partidas++;
        pthread_cond_signal(&jp->cv);
        pthread_cond_signal(&jd->cv);
        activas--;
        mesas_libres++;
        m->asignada = false;
        pthread_cond_signal(&cv_mm);
        pthread_mutex_unlock(&srv_mtx);
    }
}

static void despedir(int j, const char *resultado) {
    jugadores[j].resultado = resultado;
    pthread_cond_signal(&jugadores[j].cv);
}

static void *hilo_matchmaker(void *arg) {
    (void)arg;
    pthread_mutex_lock(&srv_mtx);
    for (;;) {
        while (!cierre && !(cola_len >= 2 && mesas_libres > 0) && !(llegados == N && cola_len < 2))
            pthread_cond_wait(&cv_mm, &srv_mtx);
        if (cierre) break;
        if (cola_len >= 2 && mesas_libres > 0) {
            int a = cola[cola_ini], b = cola[(cola_ini + 1) % N];
            cola_ini = (cola_ini + 2) % N;
            cola_len -= 2;
            int i = 0;
            while (mesas[i].asignada) i++;
            mesas[i].asignada = true;
            mesas[i].jug_puma = a;
            mesas[i].jug_perros = b;
            mesas_libres--;
            jugadores[a].en_cola = jugadores[b].en_cola = false;
            jugadores[a].mesa = jugadores[b].mesa = i;
            log_msg("[Matchmaker] Jugador %d (PUMA) vs jugador %d (PERROS) -> Mesa %d (mesas libres: %d/%d)", a, b, i,
                    mesas_libres, M);
            pthread_cond_signal(&mesas[i].cv_asignada);
            continue;
        }
        if (cola_len == 1) {
            int j = cola[cola_ini];
            cola_len = 0;
            log_msg("[Matchmaker] Jugador %d queda sin rival", j);
            despedir(j, "sin rival");
        }
        break;
    }
    while (cola_len > 0) {
        despedir(cola[cola_ini], "servidor cerrado");
        cola_ini = (cola_ini + 1) % N;
        cola_len--;
    }
    mm_fin = true;
    for (int i = 0; i < M; i++) pthread_cond_signal(&mesas[i].cv_asignada);
    pthread_mutex_unlock(&srv_mtx);
    return NULL;
}

static void *hilo_jugador(void *arg) {
    Jugador *j = arg;
    unsigned semilla = cfg.semilla + (unsigned)j->id * 17u;
    struct timespec ts;
    plazo_ms(&ts, (int)(rand_r(&semilla) % 500));
    pthread_mutex_lock(&srv_mtx);
    while (!cierre && pthread_cond_timedwait(&j->cv, &srv_mtx, &ts) != ETIMEDOUT) {
    }
    if (cierre) {
        j->resultado = "no alcanzo a entrar (servidor cerrado)";
    } else {
        cola[(cola_ini + cola_len) % N] = j->id;
        cola_len++;
        llegados++;
        j->en_cola = true;
        log_msg("[Jugador %d] llega (en cola: %d, mesas libres: %d/%d)", j->id, cola_len, mesas_libres, M);
        pthread_cond_signal(&cv_mm);
        while (j->resultado == NULL) pthread_cond_wait(&j->cv, &srv_mtx);
    }
    log_msg("[Jugador %d] termina: %s", j->id, j->resultado);
    jugadores_listos++;
    if (jugadores_listos == N) kill(getpid(), SIGUSR1);
    pthread_mutex_unlock(&srv_mtx);
    return NULL;
}

static int leer_entero(const char *s, int min, int max, int *out) {
    char *fin;
    errno = 0;
    long v = strtol(s, &fin, 10);
    if (errno || fin == s || *fin != '\0' || v < min || v > max) return -1;
    *out = (int)v;
    return 0;
}

static int cargar_config(const char *ruta) {
    cfg.semilla = 42;
    cfg.delay_min = 50;
    cfg.delay_max = 300;
    cfg.puma = nodo(2, 2);
    for (int i = 0; i < 10; i++) cfg.perros[i] = nodo(i % 5, i / 5);
    cfg.perros[10] = nodo(0, 2);
    cfg.perros[11] = nodo(4, 2);
    cfg.max_movs = 1000;
    cfg.verbose = 1;
    cfg.prob_fallo = 0;
    if (!ruta) return 0;

    FILE *f = fopen(ruta, "r");
    if (!f) {
        fprintf(stderr, "Error: no se pudo abrir %s\n", ruta);
        return -1;
    }
    char linea[256], clave[64], v1[64], v2[64];
    int nl = 0, x, y, valor;
    while (fgets(linea, sizeof linea, f)) {
        nl++;
        char *h = strchr(linea, '#');
        if (h) *h = '\0';
        int n = sscanf(linea, "%63s %63s %63s", clave, v1, v2);
        if (n <= 0) continue;
        bool es_pos = !strcmp(clave, "PUMA") || !strcmp(clave, "PERRO");
        if (es_pos) {
            if (n < 3 || leer_entero(v1, 0, 4, &x) || leer_entero(v2, 0, 4, &y)) goto error;
            if (!strcmp(clave, "PUMA")) cfg.puma = nodo(x, y);
            else if (cfg.nperros < PERROS) cfg.perros[cfg.nperros++] = nodo(x, y);
            else goto error;
            continue;
        }
        if (n < 2) goto error;
        if (!strcmp(clave, "SEED") && !leer_entero(v1, 0, 1000000000, &valor)) cfg.semilla = (unsigned)valor;
        else if (!strcmp(clave, "DELAY_MIN") && !leer_entero(v1, 1, 60000, &valor)) cfg.delay_min = valor;
        else if (!strcmp(clave, "DELAY_MAX") && !leer_entero(v1, 1, 60000, &valor)) cfg.delay_max = valor;
        else if (!strcmp(clave, "MAX_MOVIMIENTOS") && !leer_entero(v1, 0, 1000000, &valor)) cfg.max_movs = valor;
        else if (!strcmp(clave, "VERBOSE") && !leer_entero(v1, 0, 1, &valor)) cfg.verbose = valor;
        else if (!strcmp(clave, "PROB_FALLO") && !leer_entero(v1, 0, 100, &valor)) cfg.prob_fallo = valor;
        else goto error;
    }
    fclose(f);
    if (cfg.nperros != 0 && cfg.nperros != PERROS) {
        fprintf(stderr, "Error: se deben definir 0 o %d perros (hay %d)\n", PERROS, cfg.nperros);
        return -1;
    }
    if (cfg.delay_min > cfg.delay_max) {
        fprintf(stderr, "Error: DELAY_MIN (%d) mayor que DELAY_MAX (%d)\n", cfg.delay_min, cfg.delay_max);
        return -1;
    }
    bool usado[NODOS] = {false};
    usado[cfg.puma] = true;
    for (int i = 0; i < PERROS; i++) {
        if (usado[cfg.perros[i]]) {
            fprintf(stderr, "Error: dos piezas en la misma posicion inicial\n");
            return -1;
        }
        usado[cfg.perros[i]] = true;
    }
    return 0;
error:
    fprintf(stderr, "Error en %s linea %d: '%s' invalido\n", ruta, nl, clave);
    fclose(f);
    return -1;
}

int main(int argc, char **argv) {
    if (argc < 3 || argc > 4 || leer_entero(argv[1], 1, 10000, &N) || leer_entero(argv[2], 1, 1000, &M)) {
        fprintf(stderr, "Uso: %s N M [config.txt]   (N: 1..10000 jugadores, M: 1..1000 mesas)\n", argv[0]);
        return 2;
    }
    if (cargar_config(argc == 4 ? argv[3] : NULL) != 0) return 1;
    crear_tablero();

    cola = calloc((size_t)N, sizeof *cola);
    jugadores = calloc((size_t)N, sizeof *jugadores);
    mesas = calloc((size_t)M, sizeof *mesas);
    if (!cola || !jugadores || !mesas) {
        fprintf(stderr, "Error: sin memoria\n");
        return 1;
    }
    mesas_libres = M;
    for (int i = 0; i < N; i++) {
        jugadores[i].id = i;
        pthread_cond_init(&jugadores[i].cv, NULL);
    }
    for (int i = 0; i < M; i++) {
        mesas[i].id = i;
        pthread_cond_init(&mesas[i].cv_asignada, NULL);
    }

    sigset_t senales;
    sigemptyset(&senales);
    sigaddset(&senales, SIGINT);
    sigaddset(&senales, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &senales, NULL);

    log_msg("[Servidor] Torneo: N=%d jugadores, M=%d mesas, retardo %d-%d ms, semilla %u", N, M, cfg.delay_min,
            cfg.delay_max, cfg.semilla);
    for (int i = 0; i < M; i++) pthread_create(&mesas[i].juez, NULL, hilo_juez, &mesas[i]);
    pthread_create(&matchmaker, NULL, hilo_matchmaker, NULL);
    for (int i = 0; i < N; i++) pthread_create(&jugadores[i].hilo, NULL, hilo_jugador, &jugadores[i]);

    int sig;
    sigwait(&senales, &sig);
    if (sig == SIGINT) {
        log_msg("[Servidor] SIGINT recibido: cierre ordenado");
        pthread_mutex_lock(&srv_mtx);
        cierre = true;
        pthread_cond_broadcast(&cv_mm);
        for (int i = 0; i < N; i++) pthread_cond_signal(&jugadores[i].cv);
        for (int i = 0; i < M; i++) {
            if (mesas[i].en_juego) {
                lock_mesa(&mesas[i]);
                terminar(&mesas[i], 0, "servidor interrumpido (SIGINT)");
                pthread_mutex_unlock(&mesas[i].mtx);
            }
        }
        pthread_mutex_unlock(&srv_mtx);
    }

    for (int i = 0; i < N; i++) pthread_join(jugadores[i].hilo, NULL);
    pthread_join(matchmaker, NULL);
    for (int i = 0; i < M; i++) pthread_join(mesas[i].juez, NULL);

    log_msg("[Servidor] RESUMEN: partidas=%d (puma=%d, perros=%d, empates=%d, abandonos=%d, interrumpidas=%d), "
            "max mesas activas=%d/%d%s",
            partidas_jugadas, gana_puma, gana_perros, empates, abandonos, interrumpidas, max_activas, M,
            sig == SIGINT ? ", INTERRUMPIDO" : "");

    for (int i = 0; i < N; i++) pthread_cond_destroy(&jugadores[i].cv);
    for (int i = 0; i < M; i++) pthread_cond_destroy(&mesas[i].cv_asignada);
    pthread_cond_destroy(&cv_mm);
    pthread_mutex_destroy(&srv_mtx);
    free(cola);
    free(jugadores);
    free(mesas);
    log_msg("[Servidor] Recursos liberados");
    pthread_mutex_destroy(&log_mtx);
    return sig == SIGINT ? 130 : 0;
}
