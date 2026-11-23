# Inventário e Itens (módulo 2 do R7)

Código: `src/inventory.h/.c` (definições, conteúdo, pilhas, peso, equipamento, Pickups, eventos, save — no `re_render`),
`src/inventory_widget.c` (a Grade de Inventário do UMG: desenho, arrastar e soltar — junto do `ui_widget_draw.c`, no
editor e no jogo), `src/inventory_ui.c` (Details), nós em `bp_internal.h`/`blueprint.c`/`bp_vm.c`, teste
`src/inventory_test.c`. Feito do zero e modular (não usa o RPG das demos).

## Como funciona
- **Item Definition**: um Data Asset (`.rdata`) com `parent` = "Item Definition" (Content Browser > Adicionar > Definição
  de Item). Nasce com os campos Display Name, Description, Icon (textura `/Content/...`), World Mesh (nome de forma —
  Cube, Sphere... — ou vpath de malha importada), World Scale, Base Color, Max Stack, Weight (kg), Value, Tags (separadas
  por vírgula, Gameplay Tags: `Item.Consumable` faz o Use Item gastar um), Equip Slot, Socket, Attach Location/Rotation.
  O usuário pode acrescentar campos seus (lidos com Get Data Asset Value). `inv_items_refresh` lê todos ao começar o
  Play (tabela de 512, busca por hash do asset e por nome em tabela aberta, sem malloc).
- **Componente Inventory** (`CInventory`, 72 bytes: cabe nos componentes de uma classe Blueprint, limite 96): slots
  (1..64), peso máximo, pegar ao encostar, raio, tecla de interagir (E), até 6 itens iniciais. O **conteúdo** vive no
  `inventory.c` durante o Play (pool fixo de 256 inventários × 64 slots; o componente guarda o handle). Fora do Play o
  nível guarda só a configuração e os itens iniciais; o snapshot do PIE restaura o resto.
- **Regras**: Add enche primeiro as pilhas do mesmo item (até o Max Stack), depois os slots vazios; o peso corta o que
  passa do máximo (equipado também pesa). Remove tira das últimas pilhas. Move: mesmo item junta, outro troca. Split
  manda N para o primeiro slot vazio (0: metade). Sort junta as pilhas e ordena pelo nome exibido, vazios no fim.
- **Equipamento**: 8 slots com nome (Right Hand, Left Hand, Head, Chest, Legs, Feet, Back, Accessory). Equip tira um da
  bolsa (o que estava no slot volta para onde ele estava) e pendura a World Mesh como componente (`CAttach` ATTACH_SCS,
  posto pelo `bp_components_update`) na parte do Skeletal Mesh que o osso do socket move (`anim_bone_entity`; o
  padrão de cada slot: hand_r, hand_l, head, spine_03, pelvis, foot_r, spine_03, neck_01); sem esqueleto, no ator.
- **Pickup** (`CPickup` + Static Mesh; Place Actors > Pickup): sem colisão (atravessa), gira e balança; On Touch é
  pego por quem tem Auto Pickup no raio; On Interact pela tecla do jogador possuído (o mais perto). Respawn Time > 0
  esconde e volta com a quantidade inicial. O Drop Item cria um à frente do ator, ignorado por ele até ele se afastar.
- **Eventos** (fila fixa de 128, enviados no fim do `inv_tick`, dentro do `bp_tick`): On Item Used (Item), On Item
  Equipped / Unequipped (Item, Slot Name), On Inventory Changed (uma vez por quadro e inventário). Vão para a instância
  do ator e as dos seus Actor Components (`bp_fire_actor_event`). Nos scripts: métodos `OnItemUsed(item)`,
  `OnItemEquipped(item, slot)`, `OnInventoryChanged()` (script_lower.c `engine_event`); os nós entram sozinhos na API.
- **Save Game to Slot** (`online.c`): o inventário vai como mais um registro do fluxo de variáveis (`#Inventory`, que
  nenhuma variável pode ter: as versões antigas pulam). Ator sem Blueprint mas com Inventory também salva.
- **WBP_Inventory** (`inv_widget_asset`, Content/UI, criado quando falta; Content Browser > Adicionar > Widget de
  Inventário): painel, título, a Grade de Inventário com os slots (5 colunas, peso embaixo) e outra com o equipamento.
  O widget novo **Inventory Grid** (`UIW_INVENTORY`, paleta "Inventory"): Ator (vazio: o jogador), Mostra (Slots /
  Equipamento), colunas, cores. Entrada: o quadro lê o mouse contra os retângulos que o quadro anterior desenhou;
  arrastar move/troca (para outra grade: Transfer, um baú), para o equipamento equipa, para fora solta no mundo;
  botão direito usa; duplo clique equipa/desequipa; dica com nome, descrição, peso, valor, slot.

## Medidas (RTX 3060, 2560x1417, com validação)
| Cena | Resultado |
|---|---|
| `--inventory-test` (2 inventários, 257 Pickups girando e testados contra os dois a cada quadro, 2 equipados) | `inv_tick` 0,0044 ms/quadro; 424-440 fps |
| Add + Remove de 1 item (1000 vezes) | 0,064 µs o par |
| WBP_Inventory na tela (slots + equipamento + dica) | 0,033 ms o desenho de todos os widgets |

Teste `inventory` (ctest): pilhas que transbordam (9 entram, de 6 cabem 4), peso máximo (19,5 / 20 kg, a pedra não
entra), remover das últimas pilhas, usar (consumível gasta, pedra não), split, move (junta e troca), sort, find by tag,
equipar no ator e no osso hand_r do Mannequin (0,0 cm da mão), desequipar, salvar e carregar (slots e equipamento
iguais, a malha pendurada de novo), soltar e pegar de volta (não pega na hora; afastar e voltar pega), Pickup que
volta (Respawn Time), Pickup com a tecla E, os eventos pelo Blueprint (LastUsed, EquippedSlot, Changes), o
WBP_Inventory com o mouse do teste (arrastar slot 0 → 5, arrastar para Right Hand equipa, botão direito usa a poção).

## Limites / próximos passos
- Item = nome do asset (pino Name), não um pino de objeto: renomear o asset quebra os nomes escritos nos grafos.
- A Description cabe em 63 caracteres (o campo de texto do Data Asset).
- Num Skeletal Mesh só de skin (sem partes por osso: glTF/FBX), o equipado fica preso ao Skeletal Mesh (não segue o
  osso); um quadro de atraso atrás da animação (as partes são postas depois do `bp_tick`).
- Sem replicação online do inventário (o servidor teria de mandar o conteúdo) nem arrastar entre slots por toque.
- Bug visto fora do escopo: `editor.c` (`before[COMP_COUNT][128]` no Details) copia `g_components[c].size` bytes, mas
  componentes como Gameplay Tags (648 bytes) passam de 128 — escreve fora do array.
