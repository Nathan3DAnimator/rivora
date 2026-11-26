id: texture-autolink
kind: howto
title: Soltar várias texturas no Principled e ligar pelo nome | Drop several textures on the Principled and link by name | Soltar texturas sobre el Principled BSDF
keys: ligar texturas automatico, auto link textures, arrastar varias texturas, drag several textures, principled bsdf, basecolor normal roughness, pbr, mapas pbr, pbr maps, nomenclatura, naming, texturas no material, textures into material, conectar texturas, connect textures, orm, metallic roughness, substance, quixel
--- pt
```diagram steps
Selecione as texturas no Content Browser | Arraste todas juntas | Solte sobre o Principled BSDF | Cada uma na entrada certa, em coluna
```
## Como usar
- No **Content Browser**, selecione várias texturas (Ctrl ou Shift + clique) e arraste uma delas: todas vão juntas (a prévia mostra "e mais N").
- Solte **sobre um Principled BSDF** no Material Editor: cada textura vai para a entrada que o **nome** diz, organizadas em coluna na ordem das entradas:
  - **BaseColor / Albedo / Diffuse / Color** → Base Color (sRGB)
  - **Metallic / Metalness** → Metallic; **Roughness** → Roughness; **Gloss** passa por um Invert
  - **ORM / ARM** → um Separate Color: verde na Roughness, azul no Metallic
  - **Normal / NormalGL / Nrm** → um Normal Map; **NormalDX** com a convenção DirectX
  - **Emissive / Emission** → Emission Color (força 1); **Opacity / Alpha / Mask** → Alpha (o material fica Masked)
  - **AO / Height** e nomes desconhecidos ficam ao lado, sem ligar
- Os mapas que não são cor já entram como **Não-Cor** (linear). Soltas no vazio, as texturas viram uma coluna de Image Texture.
--- en
```diagram steps
Select the textures in the Content Browser | Drag them together | Drop them on the Principled BSDF | Each into its input, in a column
```
## How to use it
- In the **Content Browser**, select several textures (Ctrl or Shift + click) and drag one of them: they all go together (the preview says "and N more").
- Drop them **onto a Principled BSDF** in the Material Editor: each texture goes into the input its **name** says, organized in a column in the inputs' order:
  - **BaseColor / Albedo / Diffuse / Color** → Base Color (sRGB)
  - **Metallic / Metalness** → Metallic; **Roughness** → Roughness; **Gloss** through an Invert
  - **ORM / ARM** → a Separate Color: green into Roughness, blue into Metallic
  - **Normal / NormalGL / Nrm** → a Normal Map; **NormalDX** with the DirectX convention
  - **Emissive / Emission** → Emission Color (strength 1); **Opacity / Alpha / Mask** → Alpha (the material becomes Masked)
  - **AO / Height** and unknown names are placed beside them, unlinked
- The maps that aren't color come in as **Non-Color** (linear). Dropped on empty space, the textures become a column of Image Textures.
--- es
```diagram steps
Selecciona las texturas en el Explorador de contenidos | Arrástralas juntas | Suéltalas sobre el Principled BSDF | Cada una en su entrada, en una columna
```
## Cómo usarlo
- En el **Explorador de contenidos**, selecciona varias texturas (Ctrl o Shift + clic) y arrastra una de ellas: todas van juntas (la vista previa dice "and N more").
- Suéltalas **sobre un Principled BSDF** en el Editor de Materiales: cada textura va a la entrada que dice su **nombre**, organizadas en una columna en el orden de las entradas:
  - **BaseColor / Albedo / Diffuse / Color** → Base Color (sRGB)
  - **Metallic / Metalness** → Metallic; **Roughness** → Roughness; **Gloss** a través de un Invert
  - **ORM / ARM** → un Separate Color: verde a Roughness, azul a Metallic
  - **Normal / NormalGL / Nrm** → un Normal Map; **NormalDX** con la convención de DirectX
  - **Emissive / Emission** → Emission Color (intensidad 1); **Opacity / Alpha / Mask** → Alpha (el material pasa a ser Masked)
  - **AO / Height** y los nombres desconocidos se colocan al lado, sin enlazar
- Los mapas que no son de color entran como **Non-Color** (lineal). Soltadas en un espacio vacío, las texturas se convierten en una columna de Image Textures.
