id: collision-shapes
kind: howto
title: Formas de colisão: caixa, esfera, cápsula, convexo, malha complexa | Collision shapes: box, sphere, capsule, convex, complex mesh | Formas de colisión: caja, esfera, cápsula, convexa, malla compleja
keys: forma de colisao, collision shape, colisao convexa, convex hull, casco convexo, capsula, capsule, cilindro rola, cylinder, cone, malha complexa, use complex collision, usar malha complexa, escada, stairs, rampa, ramp, atravessa o chao, cai de lado, colide como caixa, gjk, epa, simple collision, complex collision, triangulos, triangles
--- pt
Cada ator com **Física** colide com uma forma, escolhida em **Details > Física > Forma de Colisão** (como a colisão simples da Unreal):
- **Automática** (padrão): o cubo vira caixa, a esfera vira esfera, o cilindro vira um **cilindro de verdade** (rola e fica de pé), o cone e as **malhas importadas** viram o **casco convexo** da malha (até 64 vértices, feito na primeira vez e guardado).
- **Caixa**, **Esfera**, **Cápsula** (no eixo mais comprido da malha, larga como os outros dois) ou **Convexo** (o casco de qualquer malha).
- Para o **cenário** (um ator que não simula): marque **Usar Malha Complexa** e os próprios triângulos da malha colidem: escadas, rampas, arcos, qualquer forma côncava. Só para o que fica parado; o que se move usa a forma simples.

```diagram compare
 | Caixa | Esfera / Cápsula | Convexo | Malha Complexa
Custo | o menor | baixo | médio | por triângulo
Rola | não | sim | conforme a forma | não se move
Côncavo (escada, arco) | não | não | não | sim
Para | caixotes, paredes | bolas, personagens | pedras, garrafas, móveis | cenário parado
```
```diagram steps
Selecionar o ator | Details > Adicionar > Física | Forma de Colisão: Automática, Caixa, Esfera, Cápsula ou Convexo | Cenário: desligar Simular Física e marcar Usar Malha Complexa | Play (Alt+P)
```
- Um objeto atravessa a escada ou fica "flutuando" numa rampa? A escada está com a forma simples (o casco convexo dela é uma rampa): marque **Usar Malha Complexa**.
- O Debug Viewer (F10) mostra o custo da física; o GJK e o EPA (a detecção entre formas) aparecem na medida do teste.
--- en
Each actor with **Physics** collides with a shape, chosen in **Details > Physics > Collision Shape** (like Unreal's simple collision):
- **Automatic** (default): the cube is a box, the sphere a sphere, the cylinder a **real cylinder** (it rolls and stands up), the cone and **imported meshes** the mesh's **convex hull** (up to 64 vertices, made the first time and kept).
- **Box**, **Sphere**, **Capsule** (along the mesh's longest axis, as wide as the other two) or **Convex Hull** (the hull of any mesh).
- For the **scenery** (an actor that doesn't simulate): check **Use Complex Collision** and the mesh's own triangles collide: stairs, ramps, arches, any concave shape. Only for what stays still; what moves uses its simple shape.

```diagram compare
 | Box | Sphere / Capsule | Convex Hull | Complex Mesh
Cost | lowest | low | medium | per triangle
Rolls | no | yes | as its shape | doesn't move
Concave (stairs, arch) | no | no | no | yes
For | crates, walls | balls, characters | rocks, bottles, furniture | still scenery
```
```diagram steps
Select the actor | Details > Add > Physics | Collision Shape: Automatic, Box, Sphere, Capsule or Convex Hull | Scenery: turn off Simulate Physics and check Use Complex Collision | Play (Alt+P)
```
- Something goes through the stairs or "floats" on a ramp? The stairs have their simple shape (their convex hull is a ramp): check **Use Complex Collision**.
- The Debug Viewer (F10) shows the physics' cost; GJK and EPA (the shapes' detection) appear in the test's measure.
--- es
Cada actor con **Physics** colisiona con una forma, elegida en **Detalles > Physics > Collision Shape** (como la colisión simple de Unreal):
- **Automatic** (predeterminado): el cubo es una caja, la esfera una esfera, el cilindro un **cilindro real** (rueda y se queda de pie), el cono y las **mallas importadas** el **casco convexo** de la malla (hasta 64 vértices, creado la primera vez y conservado).
- **Box**, **Sphere**, **Capsule** (a lo largo del eje más largo de la malla, tan ancha como los otros dos) o **Convex Hull** (el casco de cualquier malla).
- Para el **escenario** (un actor que no simula): marca **Use Complex Collision** y los triángulos propios de la malla colisionan: escaleras, rampas, arcos, cualquier forma cóncava. Solo para lo que se queda quieto; lo que se mueve usa su forma simple.

```diagram compare
 | Caja | Esfera / Cápsula | Casco convexo | Malla compleja
Costo | el más bajo | bajo | medio | por triángulo
Rueda | no | sí | según su forma | no se mueve
Cóncava (escalera, arco) | no | no | no | sí
Para | cajas, paredes | pelotas, personajes | rocas, botellas, muebles | escenario fijo
```
```diagram steps
Selecciona el actor | Detalles > Añadir > Physics | Collision Shape: Automatic, Box, Sphere, Capsule o Convex Hull | Escenario: desactiva Simulate Physics y marca Use Complex Collision | Jugar (Alt+P)
```
- ¿Algo atraviesa la escalera o "flota" sobre una rampa? La escalera tiene su forma simple (su casco convexo es una rampa): marca **Use Complex Collision**.
- El Debug Viewer (F10) muestra el costo de la física; GJK y EPA (la detección de las formas) aparecen en la medida de la prueba.
