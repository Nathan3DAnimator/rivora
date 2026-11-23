# Diagnóstico: GPU resetando com a simulação de tecido (08/11/2026, enviado pelo Jarvis)

## Sintomas
- RTX 3060, driver 617.42. Temperatura 55 °C e consumo de 71 W, ambos normais. GPU a 100% com o realengine.exe rodando.
- Log do sistema: evento 153 do nvlddmkm (reset/falha do canal da GPU) 9 vezes entre 10:07 e 10:14 de 08/11, também às 08:46–08:50 e várias vezes em 07/11.
- Coincide com as edições em src/cloth.c, shaders/cloth_sim.comp e src/cloth.h (10:06–10:11).

## Causa mais provável: condição de corrida nos buffers host-visible de cópia única
Em `cloth_fill`, a CPU reescreve a cada frame `C.header`, `C.info`, `C.colliders`, `C.owner` e `C.order` (mapeados, um único buffer de cada), enquanto a GPU ainda pode estar executando `cloth_record` do frame anterior (RE_FRAMES_IN_FLIGHT > 1).

Agravante: `memset(g, 0, sizeof *g)` direto em `info[n]` (memória mapeada) deixa `nx = ny = 0` por um instante. Se o shader ler nesse momento:
- `l % c.nx` e `l / c.nx` viram divisão por zero (resultado indefinido);
- `g` vira lixo, e `pos.v[c.base + gy*nx + gx]` lê fora do buffer via buffer_reference, causando falha de página na GPU e o reset do driver (TDR/evento 153).

Ao criar ou redimensionar um tecido, `owner` e `order` também são reescritos durante o frame em voo. Com isso, `ci = owner.v[v]` pode passar de MAX_CLOTH e `ClothInfoBuf(H.info).c[ci]` lê fora do buffer.

## Correção sugerida
1. Um conjunto por frame em voo: `header/info/colliders/owner/order[RE_FRAMES_IN_FLIGHT]`, escrevendo só no do frame atual (como em lights.c com `stage[RE_FRAMES_IN_FLIGHT]`). Outra opção é escrever em staging e fazer `vkCmdCopyBuffer` para buffers device-local no início de `cloth_record`, com barreira TRANSFER -> COMPUTE.
2. Montar o `ClothGpu` numa variável local e copiar de uma vez, sem `memset` direto na memória mapeada.
3. Proteções no shader (cloth_sim.comp, logo após ler `ci`/`c`):
```glsl
if (ci >= H.ncloth) return;
if (c.nx < 2u || c.ny < 2u || v < c.base || v - c.base >= c.nx * c.ny) return;
```

## Fator secundário: carga por frame
4 substeps × 24 iterações × 9 cores dão cerca de 870 dispatches com barreiras por frame. Isso não trava sozinho, mas mantém a GPU a 100% com frames longos, o que aproxima do limite de TDR (2 s) em máquinas mais lentas ou com muitos tecidos. Para testar, use `REALENGINE_CLOTH_SUBSTEPS=1` e menos iterações.

## Como confirmar
- Camada de validação do Vulkan com GPU-Assisted Validation (detecta acesso fora do limite via buffer_reference).
- Ou NVIDIA Nsight Aftermath para ver o shader e o endereço da falha.

## Resolvido (08/11/2026)
Confirmado: a cena `cloth` perdia o dispositivo em 4 de 5 execuções. Feitas as correções 1, 2 e 3 (um conjunto por quadro
em voo, `ClothInfo` montado fora da memória mapeada e copiado inteiro, proteções no `cloth_sim.comp` e no `cloth.vert`);
depois, nenhuma falha. Detalhes em [tecido.md](tecido.md).
