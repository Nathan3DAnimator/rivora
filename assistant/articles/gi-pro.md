id: gi-pro
kind: concept
title: Iluminação global (RealGI) a fundo | Global illumination (RealGI) in depth | Iluminación global (RealGI) a fondo
keys: gi, global illumination, iluminacao global, luz indireta, rebate, bounce, realgi, lumen, lightmap, sondas, probes, surface cache, ray tracing, ao, oclusao de ambiente
--- pt
O **RealGI** é híbrido:
- **Cache de superfície** em tempo real (a luz que rebate, atualizada aos poucos) e **ray tracing** por hardware quando disponível.
- **Sondas** para o que se move e **lightmaps** assados para o que é Static (Compilar > Build Lighting).
- **AO de contato** e **exposição automática** (como uma câmera).
- O modo de visualização **GI** e o **Surface Cache** mostram o que cada parte contribui.
--- en
**RealGI** is hybrid:
- A real-time **surface cache** (the bouncing light, updated a little at a time) and hardware **ray tracing** when available.
- **Probes** for what moves and baked **lightmaps** for what's Static (Build > Build Lighting).
- **Contact AO** and **auto exposure** (like a camera).
- The **GI** and **Surface Cache** view modes show what each part contributes.
--- es
**RealGI** es híbrido:
- Una **caché de superficie** en tiempo real (la luz que rebota, actualizada un poco cada vez) y **ray tracing** por hardware cuando está disponible.
- **Sondas** para lo que se mueve y **lightmaps** horneados para lo que es Static (Build > Build Lighting).
- **AO de contacto** y **exposición automática** (como una cámara).
- Los modos de vista **GI** y **Surface Cache** muestran lo que aporta cada parte.
