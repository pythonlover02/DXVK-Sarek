# VK_KHR_synchronization2 backport for DXVK-Sarek

## O que é

Backport pontual do fast path `vkCmdPipelineBarrier2KHR` (VK_KHR_synchronization2)
para o único choke point de barreiras do DXVK-Sarek 1.12.0:
`DxvkCommandList::cmdPipelineBarrier()` em `src/dxvk/dxvk_cmdlist.h`.

Ambas as chamadas existentes no codebase (`dxvk_barrier.cpp:261` e
`dxvk_context.cpp:5176`) passam por esse método sem nenhuma alteração —
a decisão de usar sync2 ou o caminho legado é feita ali, uma única vez.

## Por que

O Sarek trata `VK_KHR_synchronization2` como opcional desde antes deste
patch (via `timelineSemaphore`-style opt-in), mas nunca usava o sync2 de
fato para emitir as barreiras — apenas para outras partes do sync
subsystem. GPUs Vulkan 1.3 completas (ex: Mali-G615/Valhall em
Dimensity 7300 Ultra e superiores) suportam sync2 nativamente mas não
atendem aos 11 requisitos hard-coded do DXVK 2.x mainline (6 são
limitação de silício: `dualSrcBlend`, `fillModeNonSolid`,
`multiViewport`, `shaderClipDistance`, `shaderCullDistance`,
`textureCompressionBC` — nenhuma atualização de driver resolve; 5 são
extensões não expostas pelo driver ARM: `VK_EXT_depth_clip_enable`,
`VK_EXT_robustness2`, `VK_KHR_load_store_op_none`,
`VK_KHR_maintenance5`, `VK_KHR_maintenance6`).

Esse hardware fica preso entre "não atende ao 2.x" e "usa uma base
1.10.x que não aproveita capacidades 1.3 que ele já tem". Este patch
fecha parte dessa lacuna sem tocar em nada que dependa dos 11
requisitos ausentes.

## Como funciona

- `VK_KHR_synchronization2` é registrada como extensão **Optional**
  (nunca bloqueia a criação do device).
- `vkCmdPipelineBarrier2KHR` é carregado incondicionalmente via
  `vkGetDeviceProcAddr` no loader; a spec garante que esse ponteiro
  vem `nullptr` quando a extensão não foi habilitada no device — por
  isso a checagem `if (m_vkd->vkCmdPipelineBarrier2KHR)` é suficiente,
  sem precisar propagar uma flag de capability por `DxvkBarrierSet`
  ou `DxvkContext`.
- Conversão dos flags legados (`VkAccessFlags`/`VkPipelineStageFlags`,
  32 bits) para os equivalentes de sync2 (`VkAccessFlags2`/
  `VkPipelineStageFlags2`, 64 bits) é um widening cast simples: a
  spec Vulkan garante que os bits baixos são numericamente idênticos
  por design — não é uma conversão heurística.
- Em hardware sem sync2, o comportamento é bit-a-bit idêntico ao
  código original (`vkCmdPipelineBarrier` legado, inalterado).

## Escopo (o que este patch NÃO faz)

Não porta dynamic rendering, não muda a lógica de acumulação de
barreiras em `DxvkBarrierSet` (`dxvk_barrier.cpp`), não adiciona
timeline semaphores em filas de submit. É estritamente a troca do
mecanismo de emissão de barreira já existente por seu equivalente
sync2, quando disponível.

## Testado em

- Build limpo (0 erros) para win32 e win64, mingw-w64 13 (posix
  threading), contra DXVK-Sarek 1.12.0.
- Validação de capability cruzando contra perfil Vulkan real de um
  Mali-G615 MC2 (driver v1.r44p1-01eac0, Android 16, Vulkan 1.3.247).
- **Pendente**: teste em runtime real (GTA IV é o alvo sugerido — sem
  profile hardcoded em `config.cpp`, facilita isolar o efeito do
  patch sem interferência de tuning por jogo).

## Verificação (log real, GTA IV, Mali-G615 MC2)

Compilação limpa (0 erros) confirmada em win32 e win64. Runtime testado
via Wine debug log em GTA IV: nenhum crash, renderização correta.

**Importante**: no log analisado, `synchronization2 : 0` — o path novo
NÃO foi exercitado nesse teste específico. O motivo identificado no log
(`enumerate_physical_device:218`, aviso de wrapper Winlator) é que a
camada de passthrough Vulkan usada reporta um `VkPhysicalDeviceFeatures`
diferente do driver nativo Mali (que tem `synchronization2 = true` por
Vulkan 1.3 core, confirmado via perfil vulkan.gpuinfo.org). O patch se
comportou exatamente como projetado: detectou a feature indisponível
*nesse runtime* e caiu no fallback legado, sem quebrar nada.

Use `DXVK_HUD=sync2` (ou `DXVK_HUD=full`) para ver em tempo real, sem
precisar grep no log, se o path novo está ativo:

```
sync2: active
```
ou
```
sync2: fallback (legacy barriers)
```

## Status do repositório publicado

A tag `v1.12.0-sync2-backport` original apontava para o HEAD sincronizado
com upstream (commit `a852779`, um merge não relacionado de
`pythonlover02/DXVK-Sarek`), sem o diff deste backport aplicado.
Corrigido aplicando os 2 patches (`0001-...` + `0002-...`) via `git am`
em sequência direto nesse commit — testado e confirmado aplicar sem
conflito.


```bash
git clone --branch main --recurse https://github.com/pythonlover02/DXVK-Sarek.git DXVK
cd DXVK
git am /caminho/para/0001-Backport-VK_KHR_synchronization2-for-vkCmdPipelineBa.patch
./package-release.sh master /seu/diretorio/destino --no-package
```
