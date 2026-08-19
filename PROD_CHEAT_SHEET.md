# PoE watchdog — шпаргалка для production

## 1. Формат строки конфигурации

```text
<port> <interval_sec> <fail_threshold> <cooldown_sec> <cycle_pause_sec> <monitor_iface> <idle_threshold> [mode [watch_scope [link_timeout_sec]]]
```

Пример:

```text
G01 5 3 60 5 eth0 30 SEMIAUTO link 20
```

## 2. Что означают поля

- `port` — идентификатор PoE-порта, понятный `poe_ctl.sh`
- `interval_sec` — интервал проверки физического интерфейса
- `fail_threshold` — число повторов до сброса
- `cooldown_sec` — пауза после восстановления
- `cycle_pause_sec` — задержка между `off` и `on`
- `monitor_iface` — интерфейс, по которому смотрим carrier и трафик
- `idle_threshold` — секунд без пакетов до срабатывания
- `mode` — режим PoE после восстановления, например `AUTO`, `SEMIAUTO`, `OFF`
- `watch_scope` — `link`, `os` или `both`
- `link_timeout_sec` — таймаут без carrier до power-cycle

## 3. Стартовые значения

```text
G01 5 3 60 5 eth0 30 SEMIAUTO link 20
```

Более осторожный старт:

```text
G01 5 3 60 5 eth0 30 SEMIAUTO both 20
```

## 4. Что вызывает reset

- `link` — на `monitor_iface` пропадает `carrier` дольше `link_timeout_sec`
- `os` — трафик на `monitor_iface` прекращается на `idle_threshold`
- `both` — срабатывает любое из условий

## 5. Быстрые команды проверки

```sh
service S99poe_watchdog start
service S99poe_watchdog stop
service S99poe_watchdog restart
service S99poe_watchdog reload
```

Логи:

```sh
logread | tail -100
```

Ручной toggle PoE:

```sh
/usr/sbin/poe_ctl.sh 1 off
/usr/sbin/poe_ctl.sh 1 on
/usr/sbin/poe_ctl.sh 1 cycle
```

## 6. Чек-лист перед production

- идентификатор порта верный
- `monitor_iface` реально соответствует интерфейсу устройства
- `watch_scope` безопасен для типа устройства
- `fail_threshold` не слишком агрессивный
- `idle_threshold` не слишком мал для тихих устройств
- режим PoE корректно восстанавливается после reset

## 7. Экстренное восстановление

```sh
service S99poe_watchdog stop
/usr/sbin/poe_ctl.sh 1 on
logread | tail -200
```

## 8. Практическое правило

Сначала запускай `port`, проверь стабильность, и только потом переходи на `both`, если паттерн трафика известен и стабилен.
