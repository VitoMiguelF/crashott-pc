# userdata base dos testes

Base fixa da userdata dos testes automáticos (`tools/run_game.py`, `[paths] test_userdata` em `config/run.toml`).
Cada execução com `userdata_mode = "copy"` (o padrão de todos os testes) recebe uma **cópia** desta pasta em
`build\runs\<data>_<perfil>\userdata`, então os testes podem sobrescrever saves e opções sem mudar a base
e sem tocar na userdata do jogador (`dist\userdata`, usada só pelo perfil `jogar`).

- `content\`: os 5 slots de save que os perfis esperam (LOAD GAME no primeiro slot chega à fase com o
  Crash no episódio 2, sem cena; `menu_loaded`, `primeiro_slot_load`, `configuracoes*`...). Copiados em 2026-10-21 da
  execução `20261021-154938_primeiro_slot_load` (os mesmos saves usados por mais de 50 execuções daquele dia).
- `settings.toml`: as opções padrão do runtime (1280x720, sem upscaler, inglês, legendas desligadas, HD AI Remaster desligado).
  Para um teste com outra opção, use `CRASH_SETTINGS` no `env` do perfil, não edite este arquivo.

O cache de shaders dos testes fica à parte, em `tests\shadercache` (os shaders novos de cada execução são acrescentados lá).
Não mude esta pasta à toa: mudar os saves muda o que os perfis com botões gravados encontram nos menus.
