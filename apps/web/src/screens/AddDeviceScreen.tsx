import { useEffect, useRef, useState } from 'react';
import { useNavigate } from 'react-router-dom';
import { ChevronLeft, Radar, RefreshCw } from 'lucide-react';
import { useQueryClient } from '@tanstack/react-query';
import styles from './AddDeviceScreen.module.css';
import { Button } from '../components/Button/Button';
import { Chip } from '../components/Chip/Chip';
import { CommissioningStep } from '../components/CommissioningStep/CommissioningStep';
import { EcosystemPicker } from '../components/EcosystemPicker/EcosystemPicker';
import { EcosystemGuide } from '../components/EcosystemGuide/EcosystemGuide';
import { SetupCodeField } from '../components/SetupCodeField/SetupCodeField';
import { FoundDeviceItem } from '../components/FoundDeviceItem/FoundDeviceItem';
import { useCommissioningStore } from '../state/commissioningStore';
import type { Stage } from '../state/commissioningStore';
import { openCommissionWithCode, openDiscover, SCAN_SECONDS } from '../api/discover';
import { ecosystemById } from '../lib/ecosystem-guides';
import type { CommissioningEvent, DiscoveredDevice } from '../api/types';

const ROOMS = ['🛋 Гостиная', 'Кухня', 'Спальня', 'Ванная'];

export function AddDeviceScreen() {
  const nav = useNavigate();
  const qc = useQueryClient();
  const stage = useCommissioningStore((s) => s.stage);
  const ecosystem = useCommissioningStore((s) => s.ecosystem);
  const setupCode = useCommissioningStore((s) => s.setupCode);
  const found = useCommissioningStore((s) => s.found);
  const addedRefs = useCommissioningStore((s) => s.addedRefs);
  const currentRef = useCommissioningStore((s) => s.currentRef);
  const candidateName = useCommissioningStore((s) => s.candidateName);
  const progress = useCommissioningStore((s) => s.progress);
  const finalDevice = useCommissioningStore((s) => s.finalDevice);
  const errorMessage = useCommissioningStore((s) => s.errorMessage);
  const errorKind = useCommissioningStore((s) => s.errorKind);
  const pickEcosystem = useCommissioningStore((s) => s.pickEcosystem);
  const setSetupCode = useCommissioningStore((s) => s.setSetupCode);
  const wifiSsid = useCommissioningStore((s) => s.wifiSsid);
  const wifiPassword = useCommissioningStore((s) => s.wifiPassword);
  const setWifi = useCommissioningStore((s) => s.setWifi);
  const submitCode = useCommissioningStore((s) => s.submitCode);
  const backTo = useCommissioningStore((s) => s.backTo);
  const addFound = useCommissioningStore((s) => s.addFound);
  const chooseFound = useCommissioningStore((s) => s.chooseFound);
  const enterCodeManually = useCommissioningStore((s) => s.enterCodeManually);
  const pushEvent = useCommissioningStore((s) => s.pushEvent);
  const reset = useCommissioningStore((s) => s.reset);
  const resetAll = useCommissioningStore((s) => s.resetAll);

  useEffect(() => {
    if (stage !== 'commissioning') resetAll();
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  const handleRef = useRef<{ close: () => void } | null>(null);

  const runCommission = (code: string, eco: string) => {
    handleRef.current?.close();
    handleRef.current = openCommissionWithCode(
      code,
      eco,
      (ev: CommissioningEvent) => {
        pushEvent(ev);
      },
      // Если устройство выбрано в списке — коммишеним именно его. Код при этом
      // всё равно нужен: passcode не анонсируется.
      //
      // Имя из списка не передаём: для безымянного анонса это наша заглушка
      // («Matter device 493»), а настоящее имя бэкенд прочитает из устройства.
      {
        target: currentRef,
        // Пусто для подавляющего большинства случаев: сеть нужна только
        // устройству из коробки, которое подключается по BLE.
        wifi: wifiSsid ? { ssid: wifiSsid, password: wifiPassword } : undefined,
      },
    );
  };

  const onSubmitCode = () => {
    if (setupCode.length !== 11 || !ecosystem) return;
    submitCode();
    runCommission(setupCode, ecosystem);
  };

  useEffect(() => () => handleRef.current?.close(), []);

  const closeAndBackToHome = () => {
    reset();
    void qc.invalidateQueries({ queryKey: ['devices'] });
    nav('/');
  };

  const back = () => {
    if (stage === 'scanning') backTo('pick-ecosystem');
    else if (stage === 'enter-code') backTo('scanning');
    else if (stage === 'error') backTo('enter-code');
    else closeAndBackToHome();
  };

  return (
    <div className={styles.screen}>
      <button type="button" className={styles.back} onClick={back}>
        <ChevronLeft size={18} />
        <span>{backLabel(stage)}</span>
      </button>

      {stage === 'pick-ecosystem' && (
        <PickEcosystemStage onPick={pickEcosystem} onOther={() => alert('Скоро добавим ещё способы.')} />
      )}

      {stage === 'scanning' && (
        <ScanningStage
          found={found}
          addedRefs={addedRefs}
          onFound={addFound}
          onPick={chooseFound}
          onManual={enterCodeManually}
        />
      )}

      {stage === 'enter-code' && ecosystem && (
        <EnterCodeStage
          ecosystemId={ecosystem}
          code={setupCode}
          candidateName={candidateName}
          wifiSsid={wifiSsid}
          wifiPassword={wifiPassword}
          onWifiChange={setWifi}
          onChange={setSetupCode}
          onSubmit={onSubmitCode}
        />
      )}

      {stage === 'commissioning' && <CommissioningStage progress={progress} />}

      {stage === 'success' && finalDevice && (
        <SuccessStage
          name={finalDevice.Name}
          type={finalDevice.Type}
          onDone={closeAndBackToHome}
          onFindMore={() => resetAll()}
        />
      )}

      {stage === 'error' && (
        <FailureStage
          message={errorMessage}
          kind={errorKind}
          onRetry={() => {
            if (setupCode.length === 11 && ecosystem) {
              submitCode();
              runCommission(setupCode, ecosystem);
            } else {
              backTo('enter-code');
            }
          }}
          onOtherCode={() => {
            setSetupCode('');
            backTo('enter-code');
          }}
          onCancel={closeAndBackToHome}
        />
      )}
    </div>
  );
}

function backLabel(stage: Stage): string {
  if (stage === 'scanning' || stage === 'enter-code' || stage === 'error') return 'Назад';
  return 'Отмена';
}

function PickEcosystemStage({
  onPick,
  onOther,
}: {
  onPick: (e: import('../state/commissioningStore').Ecosystem) => void;
  onOther: () => void;
}) {
  return (
    <>
      <h1 className={styles.title}>Добавить устройство</h1>
      <p className={styles.subtitle}>Где оно уже настроено?</p>
      <EcosystemPicker onPick={onPick} onOther={onOther} />
    </>
  );
}


/**
 * Шаг сканирования: показываем всё, что сейчас анонсирует себя как готовое к
 * пейрингу.
 *
 * Найти устройство — не значит подключить его: passcode в анонсе не передаётся
 * (так устроен Matter), поэтому код с коробки всё равно понадобится на
 * следующем шаге. Список нужен, чтобы пользователь опознал своё устройство —
 * особенно когда рядом две одинаковые лампы.
 */
function ScanningStage({
  found,
  addedRefs,
  onFound,
  onPick,
  onManual,
}: {
  found: DiscoveredDevice[];
  addedRefs: Set<string>;
  onFound: (d: DiscoveredDevice) => void;
  onPick: (d: DiscoveredDevice) => void;
  onManual: () => void;
}) {
  const [scanning, setScanning] = useState(true);
  const [failed, setFailed] = useState<string | undefined>();
  const [runId, setRunId] = useState(0);
  const latestRef = useRef<string | undefined>();

  useEffect(() => {
    setScanning(true);
    setFailed(undefined);
    const handle = openDiscover(
      (d) => {
        latestRef.current = d.ref;
        onFound(d);
      },
      (e) => setFailed(e.message),
    );
    // Бэкенд закрывает поток сам по истечении окна; таймер только снимает
    // индикатор, если ответ почему-то не пришёл.
    const timer = window.setTimeout(() => setScanning(false), (SCAN_SECONDS + 2) * 1000);
    return () => {
      handle.close();
      window.clearTimeout(timer);
    };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [runId]);

  return (
    <>
      <h1 className={styles.title}>Ищем устройства</h1>
      <p className={styles.subtitle}>
        Переведи устройство в режим пейринга — оно появится в списке.
      </p>

      <div className={styles.scanList}>
        {found.map((d) => (
          <FoundDeviceItem
            key={d.ref}
            device={d}
            added={addedRefs.has(d.ref)}
            isNew={d.ref === latestRef.current}
            onAdd={() => onPick(d)}
          />
        ))}

        {scanning && (
          <div className={styles.scanRow} aria-live="polite">
            <Radar size={16} className={styles.scanIcon} />
            <span>{found.length > 0 ? 'Ищем ещё…' : 'Слушаем эфир…'}</span>
          </div>
        )}

        {!scanning && found.length === 0 && (
          <p className={styles.scanEmpty}>
            {failed
              ? `Не удалось просканировать: ${failed}`
              : 'Никого не видно. Устройство должно быть в режиме пейринга и в той же сети.'}
          </p>
        )}
      </div>

      <div className={styles.footerCol}>
        {!scanning && (
          <Button variant="primary" size="md" block onClick={() => setRunId((n) => n + 1)}>
            <RefreshCw size={16} /> Искать снова
          </Button>
        )}
        <Button variant="ghost" size="md" block onClick={onManual}>
          Ввести код вручную
        </Button>
      </div>
    </>
  );
}

function EnterCodeStage({
  ecosystemId,
  code,
  candidateName,
  wifiSsid,
  wifiPassword,
  onWifiChange,
  onChange,
  onSubmit,
}: {
  ecosystemId: import('../state/commissioningStore').Ecosystem;
  code: string;
  candidateName?: string;
  wifiSsid: string;
  wifiPassword: string;
  onWifiChange: (ssid: string, password: string) => void;
  onChange: (v: string) => void;
  onSubmit: () => void;
}) {
  const eco = ecosystemById(ecosystemId);
  if (!eco) return null;
  return (
    <>
      <h1 className={styles.title}>Введи setup-code</h1>
      <p className={styles.subtitle}>
        {candidateName
          ? `Подключаем «${candidateName}». Код не передаётся по воздуху — его нужно ввести.`
          : 'Одноразовый 11-значный код из твоего дома.'}
      </p>
      <EcosystemGuide eco={eco} />
      <div className={styles.codeBlock}>
        <SetupCodeField value={code} onChange={onChange} autoFocus />
        {/* Сеть нужна только устройству, которое ещё в неё не вошло: новому
            из коробки, подключаемому по Bluetooth. Устройство, уже видимое в
            сети, эти поля игнорирует — поэтому блок свёрнут и необязателен. */}
        <details className={styles.wifiBlock}>
          <summary className={styles.wifiSummary}>Устройство ещё не подключено к Wi-Fi?</summary>
          <p className={styles.wifiHint}>
            Заполни, если добавляешь новое устройство прямо из коробки. Оно получит эти данные
            по Bluetooth и войдёт в сеть само.
          </p>
          <input
            className={styles.wifiInput}
            type="text"
            autoComplete="off"
            placeholder="Имя сети (SSID)"
            value={wifiSsid}
            onChange={(e) => onWifiChange(e.currentTarget.value, wifiPassword)}
          />
          <input
            className={styles.wifiInput}
            type="password"
            autoComplete="off"
            placeholder="Пароль"
            value={wifiPassword}
            onChange={(e) => onWifiChange(wifiSsid, e.currentTarget.value)}
          />
        </details>

        <Button
          variant="primary"
          size="lg"
          block
          disabled={code.length !== 11}
          onClick={onSubmit}
        >
          Подключить
        </Button>
      </div>
    </>
  );
}

/**
 * Шаги соответствуют реальным стадиям коммишенинга Matter, которые присылает
 * sidecar. Несколько стадий бэкенда намеренно схлопнуты в один шаг: показывать
 * пользователю «ArmFailsafe» и «ConfigureRegulatoryInformation» отдельно незачем.
 */
const STAGE_ORDER: Array<{ key: string; also?: string[]; title: string; detail: string }> = [
  { key: 'discovering', title: 'Ищем устройство', detail: 'Matter' },
  { key: 'paired', title: 'Защищённый канал', detail: 'Обмен ключами' },
  {
    key: 'configuring',
    also: ['attesting', 'provisioning'],
    title: 'Проверяем и настраиваем',
    detail: 'Сертификат и доступы',
  },
  { key: 'network', also: ['connecting'], title: 'Подключаем к сети', detail: 'Операционный канал' },
  { key: 'finalizing', also: ['interviewing'], title: 'Читаем возможности', detail: 'Кластеры устройства' },
  { key: 'done', title: 'Готово', detail: 'Выбор комнаты' },
];

/** Индекс шага, к которому относится стадия бэкенда (или -1, если незнакомая). */
function stepIndexOf(stage: string): number {
  return STAGE_ORDER.findIndex((s) => s.key === stage || s.also?.includes(stage));
}

function CommissioningStage({ progress }: { progress: Array<{ stage: string; message?: string }> }) {
  const reachedIndex = progress.reduce((max, e) => {
    const idx = stepIndexOf(e.stage);
    return idx > max ? idx : max;
  }, -1);

  // Незнакомая стадия — не повод замереть: показываем её сообщение как есть,
  // чтобы новый шаг на бэкенде не выглядел как зависший UI.
  const lastMessage = progress.at(-1)?.message;

  return (
    <div className={styles.commissioning}>
      <div className={styles.emojiHalo}>
        <span className={styles.bigEmoji}>💡</span>
      </div>
      <h1 className={styles.centerTitle}>Подключаем устройство</h1>
      <p className={styles.centerSub}>Займёт несколько секунд</p>

      <div className={styles.steps}>
        {STAGE_ORDER.map((step, i) => {
          let status: 'idle' | 'active' | 'done' = 'idle';
          if (i < reachedIndex) status = 'done';
          else if (i === reachedIndex) status = i === STAGE_ORDER.length - 1 ? 'done' : 'active';
          return (
            <CommissioningStep
              key={step.key}
              status={status}
              title={step.title}
              detail={i === reachedIndex && lastMessage ? lastMessage : step.detail}
            />
          );
        })}
      </div>
    </div>
  );
}

function SuccessStage({
  name,
  type,
  onDone,
  onFindMore,
}: {
  name: string;
  type: string;
  onDone: () => void;
  onFindMore: () => void;
}) {
  const [room, setRoom] = useState(ROOMS[0]);
  const icon = type === 'plug' ? '🔌' : type === 'sensor' ? '🌡️' : type === 'motion' ? '👁' : '💡';
  const meta =
    type === 'light'
      ? '2700K · тёплый · 65%'
      : type === 'plug'
        ? 'Готово к работе · 0 W'
        : type === 'sensor'
          ? 'Считывает температуру'
          : 'Готово к работе';
  return (
    <div className={styles.success}>
      <div className={styles.checkHalo}>
        <span className={styles.check}>✓</span>
      </div>
      <h1 className={styles.centerTitle}>
        Готово.
        <br />
        {name} в твоём Keystone.
      </h1>

      <div className={styles.previewTile}>
        <div className={styles.previewIcon}>{icon}</div>
        <div className={styles.previewName}>{name}</div>
        <div className={styles.previewMeta}>{meta}</div>
      </div>

      <p className={styles.roomLabel}>В какую комнату?</p>
      <div className={styles.roomChips}>
        {ROOMS.map((r) => (
          <Chip key={r} tone="accent" selected={room === r} onClick={() => setRoom(r)}>
            {r}
          </Chip>
        ))}
        <Chip tone="muted">+ Новая</Chip>
      </div>

      <div className={styles.footerCol}>
        <Button variant="primary" size="lg" block onClick={onDone}>
          Готово
        </Button>
        <Button variant="ghost" size="md" block onClick={onFindMore}>
          Добавить ещё
        </Button>
      </div>
    </div>
  );
}

/**
 * Объяснения по категориям ошибок транспорта. Сырое сообщение — это Go-ошибка
 * с несколькими слоями обёрток и на английском; показывать её как основной
 * текст нельзя, но и прятать совсем не стоит: она нужна, когда пользователь
 * идёт с вопросом к нам.
 */
const FAILURE_TEXT: Record<string, string> = {
  not_found:
    'Устройство не найдено. Если ты выбирал его из списка — список мог устареть, просканируй заново.',
  unreachable:
    'Устройство не отвечает. Проверь, что оно включено и находится в той же сети.',
  timeout:
    'Устройство не ответило вовремя. Режим пейринга живёт около 60 секунд — открой его заново.',
  bad_request:
    'Код не подошёл. Проверь, что ввёл его целиком и без опечаток.',
  not_ready:
    'Matter ещё запускается. Подожди несколько секунд и попробуй снова.',
  unsupported: 'Это устройство пока не поддерживается.',
  internal: 'Внутренняя ошибка. Подробности — ниже.',
};

function FailureStage({
  message,
  kind,
  onRetry,
  onOtherCode,
  onCancel,
}: {
  message?: string;
  kind?: string;
  onRetry: () => void;
  onOtherCode: () => void;
  onCancel: () => void;
}) {
  const explanation =
    (kind && FAILURE_TEXT[kind]) ??
    'Проверь, что устройство на связи, и что «Turn on Pairing Mode» ещё активен — окно около 60 секунд.';
  return (
    <div className={styles.failure}>
      <div className={styles.emojiHalo} data-tone="ochre">
        <span className={styles.bigEmoji}>💡</span>
      </div>
      <h1 className={styles.centerTitle}>Не получилось.</h1>
      <p className={styles.centerSub}>{explanation}</p>
      {message && (
        <details className={styles.errorDetails}>
          <summary>Подробности</summary>
          <code>{message}</code>
        </details>
      )}
      <div className={styles.footerCol}>
        <Button variant="primary" size="lg" block onClick={onRetry}>
          Попробовать снова
        </Button>
        <Button variant="ghost" size="md" block onClick={onOtherCode}>
          Другой код
        </Button>
        <Button variant="ghost" size="md" block onClick={onCancel}>
          Отмена
        </Button>
      </div>
    </div>
  );
}
