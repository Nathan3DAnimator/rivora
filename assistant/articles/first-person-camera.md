id: first-person-camera
kind: recipe
title: Câmera em primeira pessoa realista: balanço, respiração, recuo, autofoco | Realistic first-person camera: bob, breathing, recoil, autofocus
keys: primeira pessoa, first person, fps, first person camera, camera em primeira pessoa, head bob, balanco da cabeca, balanco da camera, bob, passos, footstep, passo sincronizado, inercia do olhar, look lag, atraso da camera, inclinacao, lean, strafe, respiracao, breathing, cansado, exertion, esforco, medo, fear, assustado, aterrissar, landing, queda, recuo, recoil, coice, kick, autofoco, autofocus, foco automatico, profundidade de campo, depth of field, dof, bokeh, vinheta, vignette, corpo visivel, ver as pernas, own body, true first person, hide bone by name, esconder cabeca, use controller rotation yaw, preset realista, cinematico, arcade, terror, horror, set first person exertion, set first person fear, add camera recoil, set focus target, get focus distance, set first person camera preset
--- pt
A **Câmera em Primeira Pessoa** é parte do componente **Camera** (Details > Camera > **First Person Camera**): ligada, a câmera do pawn possuído se mexe como a cabeça de uma pessoa, e o que está preso nela (a arma, os braços) vai junto.
- **Balanço da cabeça no ritmo dos pés**: desce a cada pisada e vai para o lado do pé de apoio. O ritmo trava nos notifies **Footstep** da animação (os do Mannequin já têm); sem eles, vem da distância andada (**Passo**). Some no ar.
- **Inércia do olhar** (Atraso do Olhar: uma mola entre o mouse e a vista), **inclinação** nas curvas e no movimento lateral, **respiração** (mais funda cansado, mais rápida com medo), **aterrissagem** (a vista desce pela altura da queda) e **recuo** (Add Camera Recoil).
- **Autofoco**: um raio no centro da tela (ou o ator de **Set Focus Target**) e o foco indo até ele como um olho (rápido) ou uma lente (devagar); a profundidade de campo e o bokeh são os da **Câmera de Cinema** (Lente Física), ou pelo ângulo da vista e o **f-stop do Foco**.
- **Vinheta de esforço**: as bordas escurecem com o esforço (o próprio pós-processo).
- **Corpo visível**: **Mostrar o Próprio Corpo** esconde a cabeça do Skeletal Mesh (Hide Bone by Name) e você vê pernas e tronco olhando para baixo; ligue **Use Controller Rotation Yaw** no Character para o corpo olhar para onde você olha.
- **Presets**: Realista, Cinemático, Arcade, Terror. O template **Primeira Pessoa** do Project Browser já vem com o Realista, o corpo e o recuo no tiro.
- **Camera Shakes**: Adicionar > Câmera > Camera Shake (Explosão, Terremoto, Câmera na Mão, Golpe, Tiro, Pisada, Batimento, Pavor), cada canal com seno, ruído Perlin ou curva; **Start Camera Shake**, **Stop Camera Shake**, **Play World Camera Shake** (epicentro e raios).
- **Custo**: ~0,005 ms de CPU por quadro (a câmera e os shakes); Debug Viewer > First Person Camera.

```diagram steps
Camera do pawn | Details: First Person Camera | Preset Realista | Mostrar o Próprio Corpo | Play
```
```blueprint
Event Begin Play -> Set First Person Camera Preset ( Target = self, Preset = "Horror" )
Event Key E -> Set First Person Exertion ( Target = self, Exertion = 1.0 )
Event Key F -> Add Camera Recoil ( Target = self, Pitch = 2.0, Yaw = 0.3, Kick = 1.5 ) -> Start Camera Shake ( Shake = "Gunshot", Scale = 1.0 )
Event Key G -> Play World Camera Shake ( Shake = "Explosion", Epicenter = Get Actor Location ( self ), Inner Radius = 300, Outer Radius = 2000, Falloff = 1 )
```
--- en
The **First Person Camera** is part of the **Camera** component (Details > Camera > **First Person Camera**): turned on, the possessed pawn's camera moves like a person's head, and what hangs from it (the weapon, the arms) goes with it.
- **Head bob in step with the feet**: down at each foot's contact, to the stance foot's side. The rhythm locks onto the animation's **Footstep** notifies (the Mannequin's have them); without them it comes from the distance walked (**Stride**). It fades in the air.
- **The look's inertia** (Look Lag: a spring between the mouse and the view), the **lean** into turns and strafes, **breathing** (deeper when tired, quicker when afraid), the **landing** (the view dips by the fall's height) and the **recoil** (Add Camera Recoil).
- **Autofocus**: a ray at the center of the screen (or the **Set Focus Target** actor), the focus easing to it like an eye (quick) or a lens (slow); the depth of field and the bokeh are the **Cine Camera**'s (Physical Lens), or by the view's angle and the **Focus f-stop**.
- **Effort vignette**: the edges darken with the exertion (the post process' own).
- **Body seen**: **Show Own Body** hides the Skeletal Mesh's head (Hide Bone by Name) and you see your legs and torso looking down; turn on **Use Controller Rotation Yaw** on the Character so the body faces where you look.
- **Presets**: Realistic, Cinematic, Arcade, Horror. The Project Browser's **First Person** template comes with Realistic, the body and a recoil on the shot.
- **Camera Shakes**: Add > Camera > Camera Shake (Explosion, Earthquake, Handheld, Hit, Gunshot, Footstep, Heartbeat, Dread), each channel a sine, Perlin noise or a curve; **Start Camera Shake**, **Stop Camera Shake**, **Play World Camera Shake** (an epicenter and radii).
- **Cost**: ~0.005 ms of CPU a frame (the camera and the shakes); Debug Viewer > First Person Camera.

```diagram steps
The pawn's Camera | Details: First Person Camera | Realistic preset | Show Own Body | Play
```
```blueprint
Event Begin Play -> Set First Person Camera Preset ( Target = self, Preset = "Horror" )
Event Key E -> Set First Person Exertion ( Target = self, Exertion = 1.0 )
Event Key F -> Add Camera Recoil ( Target = self, Pitch = 2.0, Yaw = 0.3, Kick = 1.5 ) -> Start Camera Shake ( Shake = "Gunshot", Scale = 1.0 )
Event Key G -> Play World Camera Shake ( Shake = "Explosion", Epicenter = Get Actor Location ( self ), Inner Radius = 300, Outer Radius = 2000, Falloff = 1 )
```
