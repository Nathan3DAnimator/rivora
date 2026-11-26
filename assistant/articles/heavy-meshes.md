id: heavy-meshes
kind: concept
title: Malhas pesadas: LOD, clusters, milhões de triângulos | Heavy meshes: LOD, clusters, millions of triangles | Mallas pesadas: LOD, clusters, millones de triángulos
keys: malha pesada, muitos poligonos, milhoes de triangulos, lod, clusters, nanite, dag, otimizar modelo, highpoly, alta resolucao, instancias
--- pt
```diagram steps
Importar o modelo | Soldado e com LODs (automático) | Clusters em DAG (como o Nanite) | Perto fino, longe grosso, escondido não desenha
```
- Toda malha importada é soldada e ganha **LODs automáticos**.
- Acima de ~16 mil triângulos, os LODs viram **clusters** com culling na GPU; a fase mais nova monta um **DAG de clusters** (como o Nanite): o detalhe escolhido por pedaço, conforme o tamanho na tela.
- Milhões de instâncias: a cena fica na GPU, com culling e LOD lá (escala.md).
- Modelos de escaneamento ou esculpidos podem entrar como estão: o custo acompanha o que aparece na tela.
- Medido na cena de rochas: 5,60 → 4,62 ms. Para comparar: `REALENGINE_NO_DAG=1` e `REALENGINE_NO_DAG_OCCLUSION=1`.
--- en
```diagram steps
Import the model | Welded with LODs (automatic) | Cluster DAG (like Nanite) | Near fine, far coarse, hidden not drawn
```
- Every imported mesh is welded and gets **automatic LODs**.
- Above ~16 thousand triangles, the LODs become **clusters** culled on the GPU; the newest phase builds a **cluster DAG** (like Nanite): the detail picked per piece, by its size on screen.
- Millions of instances: the scene lives on the GPU, culled and LODed there.
- Scanned or sculpted models can go in as they are: the cost follows what shows on screen.
- Measured on the rocks scene: 5.60 → 4.62 ms. To compare: `REALENGINE_NO_DAG=1` and `REALENGINE_NO_DAG_OCCLUSION=1`.
--- es
```diagram steps
Importa el modelo | Soldado con LODs (automáticos) | DAG de clusters (como Nanite) | De cerca fino, de lejos grueso, lo oculto no se dibuja
```
- Toda malla importada se suelda y recibe **LODs automáticos**.
- Por encima de ~16 mil triángulos, los LODs se convierten en **clusters** descartados en la GPU; la fase más reciente construye un **DAG de clusters** (como Nanite): el detalle se elige por pieza, según su tamaño en pantalla.
- Millones de instancias: la escena vive en la GPU, descartada y con LOD allí mismo.
- Los modelos escaneados o esculpidos pueden entrar tal cual: el costo sigue a lo que se ve en pantalla.
- Medido en la escena rocks: 5,60 → 4,62 ms. Para comparar: `REALENGINE_NO_DAG=1` y `REALENGINE_NO_DAG_OCCLUSION=1`.
