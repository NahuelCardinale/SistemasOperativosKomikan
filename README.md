# Tarea 2: Torneo de Komikan Concurrente - Sistemas Operativos

## Integrante

* Nahuel Sánchez (Sección 04, profesor Victor Reyes)

## Uso de IA

Por falta de tiempo, usé Inteligencia Artificial para estructurar el código. Conozco los contenidos que se aplican (hilos, mutex, variables de condición y señales), comprendo cómo funciona el programa y puedo explicar cada parte.

## Compilación y Ejecución

El programa está escrito en C17 y usa hilos POSIX. Se compila con:

```
gcc -Wall -Wextra -std=c17 -pthread torneo.c -o torneo
```

Se ejecuta con `./torneo N M [config.txt]`, donde N es la cantidad de jugadores, M el máximo de mesas simultáneas y `config.txt` es opcional. Con Ctrl+C el servidor termina las partidas, libera los recursos y se cierra.

## Configuración

El archivo `config.txt` tiene una opción por línea: `SEED` (semilla), `DELAY_MIN` y `DELAY_MAX` (retardo entre movimientos, en ms), `PUMA x y` y doce líneas `PERRO x y` (posiciones iniciales), `MAX_MOVIMIENTOS` (límite antes de declarar empate), `VERBOSE` (imprimir el tablero) y `PROB_FALLO` (porcentaje de partidas en que falla un hilo, para probar la resiliencia). Las coordenadas van de 0 a 4, con y=0 en la fila de abajo. Si un valor es inválido, el programa indica la línea del error y no arranca.

## Diseño y Sincronización

Los jugadores llegan a una cola y un matchmaker los empareja de a dos (Puma y Perros), asignándoles una de las M mesas libres. Cada mesa tiene un juez que crea los 13 hilos de las piezas, que se mueven sin turnos con retardos aleatorios.

Un mutex global protege la cola y el conteo de mesas, por lo que nunca hay más de M partidas activas, y las esperas son pasivas con variables de condición. Cada mesa tiene su propio mutex para el tablero: cada movimiento se valida y se aplica en una sola sección crítica, así que si dos piezas van a la misma casilla, solo una la obtiene. Ese mutex es robusto: si un hilo muere con él tomado, el juez lo recupera y declara ganador al rival por abandono. El Ctrl+C se recibe con `sigwait`, sin manejadores asíncronos, y los locks se toman siempre en el mismo orden para evitar deadlocks.

## Pruebas

`bash pruebas/pruebas.sh` compila el programa y verifica partidas normales, el límite de mesas, colisiones, fallos de hilos, Ctrl+C, entradas inválidas y fugas de memoria con valgrind. Los 12 casos entregados están en `pruebas/test_cases.txt`.