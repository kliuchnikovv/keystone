/**
 * Генерирует Go-таблицы из модели данных CSA, которую поставляет matter.js.
 *
 * Зачем: всё, что бралось из модели (ID кластеров, имена device type),
 * оказалось верным; всё, что писалось по памяти (енумы, писабельность
 * атрибутов, обязательность аргументов) — с ошибками. Генератор переводит
 * вторую категорию в первую.
 *
 * Принцип: ни одной тихой ошибки. Если запрошенного в манифесте нет в модели —
 * генератор падает со списком пропавшего, а не выдаёт пустую таблицу. Пустая
 * таблица означала бы «енум без значений» или «у команды нет обязательных
 * аргументов» — то есть неверное утверждение, выданное за факт.
 *
 * Запуск: npm run gen:tables
 */
import { MatterModel } from "@matter/model";
import { writeFileSync } from "node:fs";
import { execFileSync } from "node:child_process";

const OUT = "../../../internal/adapters/matter/matter_gen.go";

/**
 * Манифест: только то, что keystone действительно использует. Генерировать
 * все 141 кластер — тысячи строк, из которых нужны десятки; такую свалку
 * никто не станет читать при ревью.
 */
const MANIFEST = {
    enums: [
        ["DoorLock", "LockStateEnum"],
        ["Thermostat", "SystemModeEnum"],
        ["FanControl", "FanModeEnum"],
        ["AirQuality", "AirQualityEnum"],
        ["SmokeCoAlarm", "AlarmStateEnum"],
        ["RvcOperationalState", "OperationalStateEnum"],
        ["MediaPlayback", "PlaybackStateEnum"],
        ["EnergyEvse", "StateEnum"],
        ["EnergyEvse", "SupplyStateEnum"],
        ["ColorControl", "ColorModeEnum"],
        ["ValveConfigurationAndControl", "ValveStateEnum"],
    ],
    // Атрибуты, которые keystone читает или пишет: нужен их access (писабельность)
    // и conformance (обязателен / опционален / за фича-гейтом).
    attributes: [
        ["OnOff", "OnOff"],
        ["LevelControl", "CurrentLevel"],
        ["ColorControl", "CurrentHue"],
        ["ColorControl", "CurrentSaturation"],
        ["ColorControl", "ColorTemperatureMireds"],
        ["ColorControl", "ColorMode"],
        ["DoorLock", "LockState"],
        ["WindowCovering", "CurrentPositionLiftPercent100ths"],
        ["Thermostat", "LocalTemperature"],
        ["Thermostat", "OccupiedHeatingSetpoint"],
        ["Thermostat", "OccupiedCoolingSetpoint"],
        ["Thermostat", "SystemMode"],
        ["Thermostat", "ThermostatRunningState"],
        ["FanControl", "FanMode"],
        ["FanControl", "PercentSetting"],
        ["TemperatureControl", "TemperatureSetpoint"],
        ["ValveConfigurationAndControl", "CurrentState"],
        ["ValveConfigurationAndControl", "CurrentLevel"],
        ["RvcRunMode", "CurrentMode"],
        ["ModeSelect", "CurrentMode"],
        ["LaundryWasherMode", "CurrentMode"],
        ["DishwasherMode", "CurrentMode"],
        ["RefrigeratorAndTemperatureControlledCabinetMode", "CurrentMode"],
        ["WaterHeaterMode", "CurrentMode"],
        ["EnergyEvseMode", "CurrentMode"],
        ["MediaPlayback", "CurrentState"],
        ["EnergyEvse", "State"],
        ["EnergyEvse", "SupplyState"],
    ],
    // Команды, которые keystone отправляет: нужен полный список обязательных
    // аргументов. Неполная команда молча отвергается устройством.
    commands: [
        ["OnOff", "On"],
        ["OnOff", "Off"],
        ["OnOff", "Toggle"],
        ["LevelControl", "MoveToLevel"],
        ["LevelControl", "MoveToLevelWithOnOff"],
        ["ColorControl", "MoveToHue"],
        ["ColorControl", "MoveToSaturation"],
        ["ColorControl", "MoveToHueAndSaturation"],
        ["ColorControl", "MoveToColor"],
        ["ColorControl", "MoveToColorTemperature"],
        ["DoorLock", "LockDoor"],
        ["DoorLock", "UnlockDoor"],
        ["WindowCovering", "UpOrOpen"],
        ["WindowCovering", "DownOrClose"],
        ["WindowCovering", "StopMotion"],
        ["WindowCovering", "GoToLiftPercentage"],
        ["MediaPlayback", "Play"],
        ["MediaPlayback", "Pause"],
        ["MediaPlayback", "Stop"],
        ["MediaPlayback", "Next"],
        ["MediaPlayback", "Previous"],
        ["SmokeCoAlarm", "SelfTestRequest"],
        ["EnergyEvse", "EnableCharging"],
        ["EnergyEvse", "Disable"],
        ["ValveConfigurationAndControl", "Open"],
        ["ValveConfigurationAndControl", "Close"],
        ["TemperatureControl", "SetTemperature"],
        ["RvcRunMode", "ChangeToMode"],
        ["ModeSelect", "ChangeToMode"],
        ["LaundryWasherMode", "ChangeToMode"],
        ["DishwasherMode", "ChangeToMode"],
        ["RefrigeratorAndTemperatureControlledCabinetMode", "ChangeToMode"],
        ["WaterHeaterMode", "ChangeToMode"],
        ["EnergyEvseMode", "ChangeToMode"],
        ["Chime", "PlayChimeSound"],
    ],
};

const clusters = [...MatterModel.standard.clusters];
const problems = [];

function cluster(name) {
    const c = clusters.find((x) => x.name === name);
    if (!c) problems.push(`кластер ${name} отсутствует в модели`);
    return c;
}

/**
 * Собирает цепочку наследования кластера: сам кластер, затем его базовый и так
 * далее (RvcRunMode → ModeBase, Pm25Concentration… → ConcentrationMeasurement).
 */
function chainOf(c) {
    const chain = [];
    let current = c;
    const seen = new Set();
    while (current && !seen.has(current.name)) {
        seen.add(current.name);
        chain.push(current);
        current = current.type ? clusters.find((x) => x.name === current.type) : undefined;
    }
    return chain;
}

/**
 * Значение свойства с учётом наследования.
 *
 * Переопределение в наследнике может уточнять лишь часть полей: у
 * RvcRunMode.CurrentMode есть собственная запись, но access пустой — он берётся
 * у ModeBase. Пустое поле означает «унаследовано», а не «неизвестно», и путать
 * эти два смысла нельзя: первый даёт верный факт, второй — молчаливую ложь.
 */
function effective(c, tag, name, prop) {
    for (const link of chainOf(c)) {
        const el = [...link.children].find((e) => (tag ? e.tag === tag : true) && e.name === name);
        const value = el === undefined ? "" : String(el[prop] ?? "");
        if (value !== "") return value;
    }
    return "";
}

/**
 * Ищет элемент в кластере, поднимаясь по наследованию.
 *
 * Кластеры режимов (RvcRunMode, LaundryWasherMode) наследуют CurrentMode и
 * ChangeToMode от ModeBase, а кластеры концентраций — MeasuredValue от
 * ConcentrationMeasurement. Без разрешения наследования генератор увидел бы
 * «команда без обязательных аргументов» — и выдал бы это за факт.
 */
function childOf(c, tag, name, label) {
    if (!c) return undefined;
    for (const link of chainOf(c)) {
        const found = [...link.children].find((e) => (tag ? e.tag === tag : true) && e.name === name);
        if (found) return found;
    }
    problems.push(`${label} ${c.name}.${name} отсутствует в модели (включая наследование)`);
    return undefined;
}

/** Имена спеки (NotFullyLocked) в форму keystone (not_fully_locked). */
function snake(s) {
    return s.replace(/([a-z0-9])([A-Z])/g, "$1_$2").replace(/([A-Z])([A-Z][a-z])/g, "$1_$2").toLowerCase();
}

/** camelCase — так matter.js называет свойства и аргументы. */
function camel(s) {
    return s.charAt(0).toLowerCase() + s.slice(1);
}

// --- енумы ---
const enums = [];
for (const [clusterName, enumName] of MANIFEST.enums) {
    const c = cluster(clusterName);
    const e = childOf(c, undefined, enumName, "енум");
    if (!e) continue;
    const values = [...e.children].filter((v) => typeof v.id === "number");
    if (values.length === 0) {
        problems.push(`енум ${clusterName}.${enumName} пуст — значения не извлеклись`);
        continue;
    }
    enums.push({ key: `${clusterName}.${enumName}`, values: values.map((v) => [v.id, snake(v.name)]) });
}

// --- атрибуты: писабельность и опциональность ---
const attrs = [];
for (const [clusterName, attrName] of MANIFEST.attributes) {
    const c = cluster(clusterName);
    const a = childOf(c, "attribute", attrName, "атрибут");
    if (!a) continue;
    const access = effective(c, "attribute", attrName, "access");
    const conformance = effective(c, "attribute", attrName, "conformance");
    if (access === "") {
        problems.push(`у ${clusterName}.${attrName} пустой access — писабельность не определить`);
        continue;
    }
    attrs.push({
        key: `${clusterName}.${attrName}`,
        writable: /W/.test(access),
        // Пусто — атрибут обязателен; иначе код фичи (HS, XY, CT), от которой
        // он зависит, либо "O" для просто необязательного.
        gate: conformance === "M" ? "" : conformance,
    });
}

// --- обязательные аргументы команд ---
const commands = [];
for (const [clusterName, cmdName] of MANIFEST.commands) {
    const c = cluster(clusterName);
    const cmd = childOf(c, "command", cmdName, "команда");
    if (!cmd) continue;
    // Аргументы могут наследоваться (MoveToLevelWithOnOff наследует MoveToLevel).
    // Поля тоже могут прийти из базового кластера, а не только из базовой
    // команды: ChangeToMode у RvcRunMode определён в ModeBase.
    let fields = [...(cmd.children ?? [])];
    if (fields.length === 0) {
        for (const link of chainOf(c)) {
            const inherited = [...link.children].find((e) => e.tag === "command" && e.name === cmdName);
            if (inherited && (inherited.children ?? []).length > 0) {
                fields = [...inherited.children];
                break;
            }
        }
    }
    if (fields.length === 0 && cmd.type) {
        // Команда может наследовать поля от другой команды того же кластера —
        // так MoveToLevelWithOnOff берёт аргументы у MoveToLevel.
        const base = childOf(c, "command", cmd.type, "базовая команда");
        if (base) fields = [...(base.children ?? [])];
    }
    const required = fields
        .filter((f) => String(f.conformance ?? "") === "M")
        .map((f) => camel(f.name));
    commands.push({ key: `${clusterName}.${cmdName}`, required });
}

if (problems.length > 0) {
    console.error("Генерация прервана — модель не содержит запрошенного:\n");
    for (const p of problems) console.error("  •", p);
    console.error(
        "\nЛибо имя изменилось в новой версии matter.js, либо опечатка в манифесте.\n" +
            "Пустые таблицы не выпускаются: они выглядели бы как достоверный факт.",
    );
    process.exit(1);
}

// --- вывод ---
const q = (s) => JSON.stringify(s);
const lines = [];
lines.push("// Code generated by sidecars/matter-server/tools/gen-matter-tables.mjs. DO NOT EDIT.");
lines.push("//");
lines.push("// Источник — модель данных CSA из matter.js. Правки руками будут стёрты:");
lines.push("// поменяй манифест в генераторе и перезапусти `npm run gen:tables`.");
lines.push("");
lines.push("package matter");
lines.push("");

lines.push("// genEnums — значения енумов в форме keystone, по ключу Cluster.EnumName.");
lines.push("var genEnums = map[string]map[int]string{");
for (const e of enums) {
    lines.push(`\t${q(e.key)}: {`);
    for (const [id, name] of e.values) lines.push(`\t\t${id}: ${q(name)},`);
    lines.push("\t},");
}
lines.push("}");
lines.push("");

lines.push("// genAttribute описывает атрибут так, как его определяет спека.");
lines.push("type genAttribute struct {");
lines.push("\t// Writable: спека разрешает запись. Если false — состояние меняется");
lines.push("\t// только командой, а запись устройство молча проигнорирует.");
lines.push("\tWritable bool");
lines.push("\t// Gate: пусто — атрибут обязателен; иначе код фичи кластера (HS, XY,");
lines.push("\t// CT) или \"O\" — присутствие зависит от устройства и должно");
lines.push("\t// проверяться по его собственным спискам, а не предполагаться.");
lines.push("\tGate string");
lines.push("}");
lines.push("");
lines.push("var genAttributes = map[string]genAttribute{");
for (const a of attrs) lines.push(`\t${q(a.key)}: {Writable: ${a.writable}, Gate: ${q(a.gate)}},`);
lines.push("}");
lines.push("");

lines.push("// genRequiredArgs — обязательные аргументы команды (conformance M).");
lines.push("// Неполная команда не доходит до устройства: её отвергает валидация.");
lines.push("var genRequiredArgs = map[string][]string{");
for (const c of commands) {
    lines.push(`\t${q(c.key)}: {${c.required.map(q).join(", ")}},`);
}
lines.push("}");
lines.push("");

const outPath = new URL(OUT, import.meta.url);
writeFileSync(outPath, lines.join("\n"));

// Форматируем сразу: иначе первый же `gofmt -w` в репозитории изменит файл, и
// CI-проверка «перегенерация не даёт диффа» будет падать на выравнивании.
try {
    execFileSync("gofmt", ["-w", outPath.pathname]);
} catch (err) {
    console.error("gofmt не отработал — файл сгенерирован, но не отформатирован:", String(err));
    process.exit(1);
}

console.log(
    `Готово: ${enums.length} енумов, ${attrs.length} атрибутов, ${commands.length} команд → ${OUT}`,
);
