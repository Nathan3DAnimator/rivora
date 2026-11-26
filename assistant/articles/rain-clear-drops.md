id: rain-clear-drops
kind: concept
title: Gotas de chuva transparentes (refração, Fresnel, brilho) | Clear rain drops (refraction, Fresnel, highlight) | Gotas de lluvia transparentes (refracción, Fresnel, brillo)
keys: gotas transparentes, clear drops, gotas transparentes es, gotas brancas, white drops, gotas blancas, chuva branca, white rain, lluvia blanca, refracao da chuva, rain refraction, refraccion de la lluvia, filetes, streams, chorros, goteiras, drips, goteras, gotas caindo, falling drops, gotas cayendo, chuva realista, realistic rain, lluvia realista, REALENGINE_RAIN_FLAT
--- pt
```diagram steps
O quadro anterior (a cópia do SSR / do vidro)
Cada gota é uma lentezinha: lê o que está atrás, invertido
Fresnel na borda: o céu e a sala refletidos
Brilho pequeno: o sol e os postes espalhados pela gota
```
As **gotas caindo**, as **goteiras** e os **filetes** que caem das beiradas usam o mesmo shading das gotas paradas nas paredes e janelas: água transparente, nunca um traço branco chapado.
- **Refração**: cada gota é uma lente pequena que mostra o que está atrás dela invertido (a cena do quadro anterior, a mesma cópia do SSR e do vidro, feita em todo quadro com chuva).
- **Fresnel**: na borda a gota reflete o céu (ou a sala, debaixo de um telhado); no meio ela é quase só transparência.
- **Brilho**: o sol e as luzes próximas espalhados para a frente viram um destaque pequeno fora do centro (contra a luz a chuva brilha, contra o céu claro quase some, como na vida).
- O estilo **Estilizado** da chuva mantém os traços claros de antes. Para comparar com o shading antigo: `REALENGINE_RAIN_FLAT=1`.
- Custo (RTX 3060, 1440p Épico, cena `rain`): ~0 ms (2,19 → 2,20 ms; a cópia da cena já existia com o SSR ligado).
- Veja também: [Shading da chuva](article:rain-shading), [Chuva e água](article:rain-water).
--- en
```diagram steps
Last frame (the SSR's / the glass's copy)
Each drop a tiny lens: it reads what is behind it, upside down
Fresnel at the rim: the sky and the room reflected
A small highlight: the sun and the street lamps scattered by the drop
```
The **falling drops**, the **drips** and the **streams** off the edges use the same shading as the drops standing on walls and windows: clear water, never a flat white streak.
- **Refraction**: each drop is a small lens showing what is behind it upside down (the scene of the last frame, the same copy as the SSR's and the glass's, made every frame while it rains).
- **Fresnel**: at the rim a drop reflects the sky (or the room, under a roof); in the middle it is almost all see-through.
- **Highlight**: the sun and the nearby lamps scattered forward make a small highlight off center (against the light the rain shines, against a bright sky it nearly vanishes, as in life).
- The rain's **Stylized** style keeps the bright lines of before. To compare with the old shading: `REALENGINE_RAIN_FLAT=1`.
- Cost (RTX 3060, 1440p Epic, the `rain` scene): ~0 ms (2.19 → 2.20 ms; the scene's copy was already made with SSR on).
- See also: [Rain shading](article:rain-shading), [Rain and water](article:rain-water).
--- es
```diagram steps
Último fotograma (la copia del SSR / del cristal)
Cada gota, una lente diminuta: lee lo que hay detrás, al revés
Fresnel en el borde: el cielo y la habitación reflejados
Un pequeño brillo: el sol y las farolas dispersados por la gota
```
Las **gotas que caen**, los **goteos** y los **chorros** de los bordes usan el mismo sombreado que las gotas quietas en paredes y ventanas: agua clara, nunca una raya blanca plana.
- **Refracción**: cada gota es una pequeña lente que muestra lo que hay detrás, al revés (la escena del último fotograma, la misma copia del SSR y del cristal, hecha en cada fotograma mientras llueve).
- **Fresnel**: en el borde una gota refleja el cielo (o la habitación, bajo un techo); en el centro es casi toda transparente.
- **Brillo**: el sol y las lámparas cercanas, dispersados hacia delante, hacen un pequeño brillo descentrado (a contraluz la lluvia brilla, contra un cielo luminoso casi desaparece, como en la vida real).
- El estilo **Stylized** de la lluvia conserva las líneas brillantes de antes. Para comparar con el sombreado antiguo: `REALENGINE_RAIN_FLAT=1`.
- Coste (RTX 3060, 1440p Épico, la escena `rain`): ~0 ms (2.19 → 2.20 ms; la copia de la escena ya se hacía con el SSR activado).
- Mira también: [Sombreado de la lluvia](article:rain-shading), [Lluvia y agua](article:rain-water).
