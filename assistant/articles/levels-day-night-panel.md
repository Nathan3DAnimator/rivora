id: levels-day-night-panel
kind: howto
title: Dia e noite com Lighting Levels, e o painel Níveis | Day and night with Lighting Levels, and the Levels panel
keys: dia e noite, day and night, ceu de dia, ceu de noite, predefinicao de ceu, sky preset, lighting level dia, lighting level noite, day lighting level, night lighting level, nao soma, does not add, nivel preto, black level, nivel escuro ao reabrir, level black after reopening, lightmap preto, black lightmap, neblina por nivel, fog per level, nuvens por nivel, clouds per level, post process por nivel, post process per level, sky atmosphere por nivel, remover subnivel, remove sublevel, apagar subnivel, delete sublevel, desfazer remover nivel, undo remove level, reordenar niveis, reorder levels, arrastar nivel, drag level, organizador por nivel, outliner by level, atores do nivel, level actors, todos os niveis, all levels, cor do nivel, level color, cartoes de nivel, level cards, nivel de dados, data level
--- pt
O caso clássico: um **nível persistente sem luz** (só a geometria) e dois Lighting Levels, **Day** e **Night**, cada um com o próprio céu. Ligue um, desligue o outro, ou misture os dois.
1. **Janela > Níveis > Criar Novo...** com **Lighting Level** marcado: "Day". Ele já fica como o **atual** (borda destacada, "atual" no cartão).
2. Com o Day atual, **Adicionar > Efeitos Visuais > Sky Preset > Clear Day**. A predefinição cria **no nível atual** o sol, o Sky Light, a neblina e as nuvens (o que já existe **naquele nível** é ajustado; nada de outro nível é tocado).
3. Crie "Night" do mesmo jeito e aplique **Night (Moonlight)**: a lua, o céu, a neblina e as nuvens da noite vão para o Night, e o sol do Day continua o mesmo.
4. **Build > Build Lighting**: um bake do persistente e um de cada Lighting Level **só com o que ilumina nele** (luzes, céu, nuvens, neblina, volumes de neblina e Post Process Volume do próprio nível).
5. O **olho** do cartão liga e desliga; o campo **mistura** (0 a 2) dá o peso. Tudo o que o nível leva segue o peso junto: as luzes, o Sky Light, o sol que pinta o céu, a neblina (as densidades somam), as nuvens, os Post Process Volumes e a luz baked das malhas.
- **Reabrir** o projeto mostra a mesma combinação (visível e peso ficam salvos) e as malhas acham o lightmap mesmo quando os atores voltam com outro identificador (os subníveis entram depois do persistente).
- **Painel Níveis**: cada nível é um **cartão** com a cor dele (faixa à esquerda), o olho, o ícone do tipo (**sol** = Lighting Level, **dados** = nível de dados; clique para trocar), o nome, a contagem de atores, **No início**, o cadeado de **travar**, a cor e a **lixeira**.
- **Clique** num cartão: o **Organizador** mostra só os atores daquele nível, com "Nível: Day" no topo (escolha **Todos os Níveis** para ver tudo). **Clique duplo**: vira o nível atual. **Arraste** um cartão para outro lugar para reordenar; **arraste atores** do Organizador para um cartão para movê-los para aquele nível.
- **Remover**: a lixeira do cartão, o botão **Remover** no topo, o clique direito > Remover Nível Selecionado ou **Del** com o painel em foco. Sempre pergunta antes; os atores saem do mundo, o arquivo `.rlevel` fica no projeto, e **Ctrl+Z** traz o nível de volta no mesmo lugar (Ctrl+Y remove de novo).

```diagram steps
Criar Novo "Day" (Lighting) | Sky Preset Clear Day | Criar Novo "Night" (Lighting) | Sky Preset Night | Build Lighting | olho ou mistura em cada cartão
```
```blueprint
Event Begin Play -> Set Lighting Level Visible ( Level Name = "Night", Visible = false )
Event Key N -> Timeline ( Play from Start )
Timeline.Update -> Set Lighting Level Blend ( Level Name = "Night", Blend = Timeline.Alpha )
```
--- en
The classic case: a **persistent level with no light** (geometry only) and two Lighting Levels, **Day** and **Night**, each with its own sky. Turn one on and the other off, or blend the two.
1. **Window > Levels > Create New...** with **Lighting Level** ticked: "Day". It becomes the **current** level (highlighted border, "current" on its card).
2. With Day current, **Add > Visual Effects > Sky Preset > Clear Day**. The preset makes the sun, the Sky Light, the fog and the clouds **in the current level** (what already exists **in that level** is adjusted; nothing in another level is touched).
3. Make "Night" the same way and apply **Night (Moonlight)**: the moon, the sky, the fog and the clouds of the night go into Night, and Day's sun stays as it was.
4. **Build > Build Lighting**: a bake of the persistent level and one of each Lighting Level **with only what lights in it** (its own lights, sky, clouds, fog, fog volumes and Post Process Volumes).
5. The card's **eye** turns it on and off; the **blend** field (0 to 2) is its weight. Everything the level carries follows the weight together: its lights, its Sky Light, the sun that paints the sky, its fog (the densities add), its clouds, its Post Process Volumes and the meshes' baked light.
- **Reopening** the project shows the same mix (visibility and weight are saved), and the meshes find their lightmaps even when the actors come back under another identifier (the sublevels come in after the persistent level).
- **Levels panel**: each level is a **card** in its color (the stripe on the left), with its eye, its kind icon (**sun** = Lighting Level, **data** = data level; click to switch), its name, its actor count, **At start**, the **lock**, its color and the **trash can**.
- **Click** a card: the **Outliner** lists only that level's actors, with "Level: Day" on top (pick **All Levels** to see everything). **Double-click**: it becomes the current level. **Drag** a card to another place to reorder; **drag actors** from the Outliner onto a card to move them into that level.
- **Remove**: the card's trash can, the **Remove** button on top, right-click > Remove Selected Level, or **Del** with the panel focused. It always asks first; the actors leave the world, the `.rlevel` file stays in the project, and **Ctrl+Z** brings the level back in the same place (Ctrl+Y removes it again).

```diagram steps
Create New "Day" (Lighting) | Sky Preset Clear Day | Create New "Night" (Lighting) | Sky Preset Night | Build Lighting | the eye or the blend on each card
```
```blueprint
Event Begin Play -> Set Lighting Level Visible ( Level Name = "Night", Visible = false )
Event Key N -> Timeline ( Play from Start )
Timeline.Update -> Set Lighting Level Blend ( Level Name = "Night", Blend = Timeline.Alpha )
```
