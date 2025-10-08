# Pub/Sub en C — TCP y UDP (Laboratorio 3)

Implementación **desde cero en C** del modelo Publicador–Suscriptor con **Broker** intermedio,
en dos versiones: **TCP** y **UDP**.

> Requisitos del lab: implementar en **C**, organizar archivos como `broker_tcp.c`, `publisher_tcp.c`, `subscriber_tcp.c` y sus equivalentes en UDP. Compilar con `gcc` y ejecutar en Linux.

## Estructura
```
broker_tcp.c
publisher_tcp.c
subscriber_tcp.c
broker_udp.c
publisher_udp.c
subscriber_udp.c
Makefile
```

## Compilar
Desde Linux (o WSL en Windows):
```bash
make clean && make
```

Esto generará los binarios:
```
broker_tcp, publisher_tcp, subscriber_tcp,
broker_udp, publisher_udp, subscriber_udp
```

## Ejecutar (TCP)
En 3–4 terminales distintas:

1) Broker (puerto 5000 por defecto razonable; se puede cambiar por argumento):
```bash
./broker_tcp 5000
```

2) Uno o más Suscriptores: (IP del broker, puerto y 1..N temas)
```bash
./subscriber_tcp 127.0.0.1 5000 "EQUIPO_A" "EQUIPO_B"
```

3) Uno o más Publicadores: (IP del broker, puerto, tema y cantidad de mensajes)
```bash
./publisher_tcp 127.0.0.1 5000 "EQUIPO_A" 10
```

> Cada publicador enviará N mensajes (con número incremental) con pausas de ~1s.

## Ejecutar (UDP)
Use un **único puerto** para todo (por ejemplo 6000). En 3–4 terminales:

1) Broker UDP:
```bash
./broker_udp 6000
```

2) Suscriptores (registran su suscripción enviando `SUB <tema>` al broker y quedan escuchando):
```bash
./subscriber_udp 127.0.0.1 6000 "EQUIPO_A" "EQUIPO_B"
```

3) Publicadores (envían datagramas `PUB <tema> <mensaje>`):
```bash
./publisher_udp 127.0.0.1 6000 "EQUIPO_A" 10
```

## Probar con Wireshark
- **TCP**: filtre por `tcp.port == 5000`
- **UDP**: filtre por `udp.port == 6000`

Guarde capturas como `tcp_pubsub.pcap` y `udp_pubsub.pcap` para el informe.

## Notas
- El **Broker TCP** usa `select()` para multiplexar múltiples conexiones y maneja líneas separadas por `\n`.
- El **Broker UDP** mantiene una tabla de suscriptores por tema con direcciones `(IP, puerto)` y reenvía a todos los suscritos.
- Los mensajes son simples y en texto:  
  - `SUB <tema>` para suscribirse (TCP/UDP)  
  - `PUB <tema> <mensaje...>` para publicar (TCP/UDP)

## Errores comunes
- Asegúrate de abrir puertos no usados y de permitir tráfico en el firewall.
- Si usas WSL, corre el Broker y clientes dentro de WSL para simplificar.
- Recuerda usar comillas para temas que contengan espacios.
