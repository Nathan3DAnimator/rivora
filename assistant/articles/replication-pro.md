id: replication-pro
kind: concept
title: Replicação: RPCs e variáveis replicadas | Replication: RPCs and replicated variables | Replicación a fondo: RPCs y variables replicadas
keys: repnotify, onrep, replicates, reliable, not replicated, run on owning client, replicacao, replication, rpc, run on server, multicast, owning client, variavel replicada, autoridade, authority, servidor, cliente, netcode, lag, perda de pacote
--- pt
```diagram compare
 | Quem chama | Onde roda
Run on Server | um cliente | o servidor
Multicast | o servidor | todos
Run on Owning Client | o servidor | o dono do ator
```
- Protocolo próprio sobre UDP com acks, canais confiáveis e não confiáveis e um emulador de lag e perda (para testar).
- Characters e corpos com física já sincronizam; outros atores: **Set Replicates**.
- **RPCs:** Run on Server (cliente pede), Multicast (servidor avisa todos), Run on Owning Client.
- **Variáveis replicadas** e **Has Authority** para decidir quem manda.
- Os clientes interpolam o movimento: fica liso mesmo com lag.

## Como na Unreal: direto na variável e no evento
- **Variável** (My Grafo > a variável > Details > **Replication**): *None*, *Replicated* (mudou no servidor, vai para todos os clientes) ou *RepNotify* (vai e ainda roda o Custom Event **OnRep_Nome**, criado sozinho no Event Graph; roda no servidor também).
- **Custom Event** (Details > **Replicates**): *Multicast*, *Run on Server*, *Run on owning Client* e **Reliable**. Chamar o evento já envia; o título do nó mostra onde ele roda. O primeiro Float, Vector e Text da chamada viajam: do outro lado leia com **Get RPC Float / Vector / Text**.
- Os nós **Run on Server / Multicast / Replicate Variable** continuam funcionando para quem já usa.
--- en
```diagram compare
 | Who calls | Where it runs
Run on Server | a client | the server
Multicast | the server | everyone
Run on Owning Client | the server | the actor's owner
```
- An own protocol over UDP with acks, reliable and unreliable channels and a lag and loss emulator (for testing).
- Characters and physics bodies sync already; other actors: **Set Replicates**.
- **RPCs:** Run on Server (a client asks), Multicast (the server tells everyone), Run on Owning Client.
- **Replicated variables** and **Has Authority** to decide who's in charge.
- Clients interpolate movement: smooth even with lag.

## As in Unreal: on the variable and on the event
- **Variable** (My Graph > the variable > Details > **Replication**): *None*, *Replicated* (changed on the server, sent to every client) or *RepNotify* (sent, and it runs the Custom Event **OnRep_Name**, made for you in the Event Graph; on the server too).
- **Custom Event** (Details > **Replicates**): *Multicast*, *Run on Server*, *Run on owning Client* and **Reliable**. Calling the event sends it; the node's title shows where it runs. The call's first Float, Vector and Text travel: read them there with **Get RPC Float / Vector / Text**.
- The **Run on Server / Multicast / Replicate Variable** nodes still work for graphs that use them.
--- es
```diagram compare
 | Quién llama | Dónde se ejecuta
Run on Server | un cliente | el servidor
Multicast | el servidor | todos
Run on Owning Client | el servidor | el dueño del actor
```
- Un protocolo propio sobre UDP con acks, canales fiables y no fiables y un emulador de lag y pérdida (para pruebas).
- Los Characters y los cuerpos físicos ya se sincronizan; otros actores: **Set Replicates**.
- **RPCs:** Run on Server (un cliente pide), Multicast (el servidor avisa a todos), Run on Owning Client.
- **Variables replicadas** y **Has Authority** para decidir quién manda.
- Los clientes interpolan el movimiento: fluido incluso con lag.

## Como en Unreal: en la variable y en el evento
- **Variable** (My Graph > la variable > Detalles > **Replication**): *None*, *Replicated* (cambia en el servidor, se envía a todos los clientes) o *RepNotify* (se envía y ejecuta el Custom Event **OnRep_Nombre**, creado para ti en el Event Graph; también en el servidor).
- **Custom Event** (Detalles > **Replicates**): *Multicast*, *Run on Server*, *Run on owning Client* y **Reliable**. Llamar al evento lo envía; el título del nodo muestra dónde se ejecuta. El primer Float, Vector y Text de la llamada viajan: léelos allí con **Get RPC Float / Vector / Text**.
- Los nodos **Run on Server / Multicast / Replicate Variable** siguen funcionando para los grafos que los usan.
