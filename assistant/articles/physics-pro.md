id: physics-pro
kind: recipe
title: Física de corpos rígidos: pilhas, CCD, materiais, juntas 6DOF | Rigid body physics: stacks, CCD, materials, 6DOF joints
keys: rigid body, corpo rigido, fisica pesada, heavy physics, milhares de caixas, thousands of boxes, pilha, stack, piramide, pyramid, tremer, jitter, afundar, sink, substeps, sub-passos, solver substeps, ccd, continuous collision, colisao continua, bala atravessa parede, bullet through wall, tunneling, material fisico, physical material, atrito, friction, quique, bounce, restitution, densidade, density, gelo, ice, borracha, rubber, 6dof, cone, twist, swing, limite mole, soft limit, drive, mola, spring, motor, add force, add torque, set linear drive, set angular drive, get constraint force, wake rigid body, put rigid body to sleep, dormir, sleep, ilhas, islands, physics quality, qualidade da fisica, physics rate, taxa da fisica, 60 hz, 120 hz, 20 mil corpos, 20000 bodies, fisica lenta, slow physics, desempenho da fisica, physics performance, fps com fisica
--- pt
A física de corpos rígidos resolve **milhares de corpos em paralelo** (todas as threads), com **sub-passos** (o "soft step" do Box2D v3 em 3D): pilhas altas ficam paradas e nada treme.
1. **Pilhas**: Project Settings > Physics > **Solver Substeps** (4 por padrão; 6 a 8 para torres de dezenas de caixas). Um monte parado **dorme inteiro** (ilhas) e não custa nada até algo encostar nele.
2. **Balas e coisas rápidas**: Details > Física > **Usar CCD**. O corpo é varrido de onde estava até onde vai e para na primeira parede, por mais fina que seja (testado a 1000 m/s numa parede de 2 cm). Corpos mais rápidos que metade do próprio tamanho por passo já são varridos contra o cenário sozinhos.
3. **Material Físico**: Navegador de Conteúdo > Adicionar > **Física** > Material Físico (Madeira, Metal, Pedra, Borracha, Gelo, Plástico, Vidro). Atrito, Restituição, **Densidade** (a massa quando Massa é 0) e como dois se combinam. Ponha em Details > Física > **Material Físico**.
4. **Juntas** (Physics Constraint): além de Fixa, Dobradiça, Esfera e Prismática, **Cone e Torção** (um cone de Balanço 1 x Balanço 2 e um limite de torção: ombro, corrente) e **6DOF** (cada eixo Livre, Limitado ou Travado; **limites moles** com mola; **drives** linear e angular que puxam até um alvo com Frequência e Amortecimento; Força e Torque de Quebra).
5. **Blueprint**: Add Force, Add Torque in Degrees, Add Impulse at Location, Set Physics Linear/Angular Velocity, Set Mass Override in Kg, Wake Rigid Body, Put Rigid Body to Sleep, Set Physical Material, Set Use CCD; nas juntas Set Linear Drive, Set Angular Drive, Set Constraint Limits, Set Constraint Axis Motion e Get Constraint Force.
6. O custo de cada fase (broadphase, narrowphase, solver) aparece no Debug Viewer (F10) > Physics.
7. **Milhares de corpos acordados ao mesmo tempo**: Project Settings > Physics > **Qualidade da Física**. **Épico** (120 passos por segundo x 4 sub-passos, o padrão), **Alto** (90 x 4), **Médio** (60 x 4, o padrão do Box2D: corta o custo da física pela metade; uma pilha alta afunda uns cm a mais) e **Baixo** (60 x 2). É do projeto, igual em toda máquina (não muda com os gráficos do jogador). Medido num armazém de 20 mil caixotes com explosões (RTX 3060, 1440p, gráficos Épico): Épico 143 fps, Médio 163 fps.

```diagram steps
Corpo com Simular Física | Material Físico: Borracha (quica) | Usar CCD se for rápido | Physics Constraint 6DOF: X Livre, drive de posição 2 Hz | Play: o corpo vai até o alvo e quica no chão
```
```blueprint
Event BeginPlay
  -> Set Physical Material ( Target = self, Material = "/Content/PM_Rubber.rphysmat" )
  -> Set Use CCD ( Target = self, Enable = true )
  -> Add Force ( Target = self, Force = Make Vector( 0, 0, 50000 ), Accel Change = false )
```
```real
class Thruster extends Actor
    event BeginPlay()
        SetUseCCD(true)

    event Tick(dt: float)
        AddForce(Vector(0, 0, 1200), true)
```
--- en
Rigid body physics solves **thousands of bodies in parallel** (every thread), with **substeps** (Box2D v3's "soft step" in 3D): tall stacks stand still and nothing trembles.
1. **Stacks**: Project Settings > Physics > **Solver Substeps** (4 by default; 6 to 8 for towers of dozens of boxes). A pile at rest **sleeps whole** (islands) and costs nothing until something touches it.
2. **Bullets and fast things**: Details > Physics > **Use CCD**. The body is swept from where it was to where it goes and stops at the first wall, however thin (tested at 1000 m/s against a 2 cm wall). Bodies faster than half their own size a step are swept against the level by themselves.
3. **Physical Material**: Content Browser > Add > **Physics** > Physical Material (Wood, Metal, Stone, Rubber, Ice, Plastic, Glass). Friction, Restitution, **Density** (the mass when Mass is 0) and how two combine. Set it in Details > Physics > **Physical Material**.
4. **Joints** (Physics Constraint): besides Fixed, Hinge, Ball and Prismatic, **Cone and Twist** (a Swing 1 x Swing 2 cone and a twist limit: a shoulder, a chain) and **6DOF** (each axis Free, Limited or Locked; **soft limits** with a spring; linear and angular **drives** pulling to a target with Frequency and Damping; Break Force and Torque).
5. **Blueprint**: Add Force, Add Torque in Degrees, Add Impulse at Location, Set Physics Linear/Angular Velocity, Set Mass Override in Kg, Wake Rigid Body, Put Rigid Body to Sleep, Set Physical Material, Set Use CCD; for joints Set Linear Drive, Set Angular Drive, Set Constraint Limits, Set Constraint Axis Motion and Get Constraint Force.
6. Each phase's cost (broadphase, narrowphase, solver) shows in the Debug Viewer (F10) > Physics.
7. **Thousands of bodies awake at once**: Project Settings > Physics > **Physics Quality**. **Epic** (120 steps a second x 4 substeps, the default), **High** (90 x 4), **Medium** (60 x 4, Box2D's own default: halves the physics' cost; a tall pile sinks a few cm more) and **Low** (60 x 2). It is the project's, the same on every machine (it doesn't change with the player's graphics). Measured in a warehouse of 20 thousand crates with explosions (RTX 3060, 1440p, Epic graphics): Epic 143 fps, Medium 163 fps.

```diagram steps
Body with Simulate Physics | Physical Material: Rubber (bounces) | Use CCD if it is fast | 6DOF Physics Constraint: X Free, a 2 Hz position drive | Play: the body goes to the target and bounces on the floor
```
```blueprint
Event BeginPlay
  -> Set Physical Material ( Target = self, Material = "/Content/PM_Rubber.rphysmat" )
  -> Set Use CCD ( Target = self, Enable = true )
  -> Add Force ( Target = self, Force = Make Vector( 0, 0, 50000 ), Accel Change = false )
```
```real
class Thruster extends Actor
    event BeginPlay()
        SetUseCCD(true)

    event Tick(dt: float)
        AddForce(Vector(0, 0, 1200), true)
```
