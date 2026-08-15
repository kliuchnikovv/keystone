/**
 * Ручные типы до момента, когда `yarn gen:proto` сгенерирует их из proto/keystone/v1/*.proto.
 * Go-энкодер сейчас пишет поля в CapitalCase — сохраняем оригинал в `Raw*`,
 * а в приложении используем camelCase алиасы через `toDevice()` / `toStateEvent()`.
 */

export type DeviceType = 'light' | 'plug' | 'sensor' | 'motion' | 'switch' | string;
export type Transport = 'virtual' | 'matter' | 'zigbee' | 'wifi' | string;

export interface RawFeature {
  Key: string;
  States?: string[];
  Actions?: string[];
}

export interface RawDevice {
  ID: string;
  Type: DeviceType;
  Name: string;
  Manufacturer?: string;
  Model?: string;
  Room?: string;
  Transport: Transport;
  TransportRef?: string;
  Features?: RawFeature[];
  CreatedAt?: string;
  UpdatedAt?: string;
}

export interface Feature {
  key: string;
  states: string[];
  actions: string[];
}

export interface Device {
  id: string;
  type: DeviceType;
  name: string;
  manufacturer?: string;
  model?: string;
  room?: string;
  transport: Transport;
  transportRef?: string;
  features: Feature[];
  createdAt?: string;
  updatedAt?: string;
}

export function toDevice(raw: RawDevice): Device {
  return {
    id: raw.ID,
    type: raw.Type,
    name: raw.Name,
    manufacturer: raw.Manufacturer,
    model: raw.Model,
    room: raw.Room || undefined,
    transport: raw.Transport,
    transportRef: raw.TransportRef,
    features: (raw.Features ?? []).map((f) => ({
      key: f.Key,
      states: f.States ?? [],
      actions: f.Actions ?? [],
    })),
    createdAt: raw.CreatedAt,
    updatedAt: raw.UpdatedAt,
  };
}

export interface RawStatePayload {
  DeviceID: string;
  Feature: string;
  Key: string;
  Value: unknown;
  UpdatedAt?: string;
  Origin?: string;
}

export interface RawEventPayload {
  DeviceID: string;
  Feature: string;
  Name: string;
  Data?: Record<string, unknown>;
  At?: string;
}

export type StreamMessage =
  | { type: 'state'; payload: RawStatePayload }
  | { type: 'event'; payload: RawEventPayload }
  | { type: string; payload: unknown };

export interface DiscoveredDevice {
  ref: string;
  name: string;
  /** Пусто, если устройство не сообщило свой тип в анонсе. */
  type?: DeviceType;
  transport: Transport;
  manufacturer?: string;
  features?: Feature[];
  /**
   * Длинный дискриминатор Matter. Он же напечатан рядом с QR-кодом на
   * устройстве — единственное, по чему пользователь может отличить две
   * одинаковые лампы в списке.
   */
  discriminator?: number;
  vendorId?: number;
  productId?: number;
}

/**
 * Стадии приходят из sidecar'а и соответствуют реальным шагам коммишенинга
 * Matter, а не таймерам. Список открытый: бэкенд может добавить шаг, и UI не
 * должен от этого ломаться — незнакомые стадии просто показываются как есть.
 */
export type CommissioningStage =
  | 'discovering'
  | 'paired'
  | 'configuring'
  | 'attesting'
  | 'provisioning'
  | 'network'
  | 'connecting'
  | 'finalizing'
  | 'interviewing'
  | 'done'
  | 'error'
  | (string & {});

/** Категория ошибки от транспорта — по ней UI выбирает объяснение. */
export type CommissioningErrorKind =
  | 'bad_request'
  | 'not_found'
  | 'not_ready'
  | 'unreachable'
  | 'timeout'
  | 'unsupported'
  | 'internal'
  | (string & {});

export interface CommissioningEvent {
  stage: CommissioningStage;
  message?: string;
  kind?: CommissioningErrorKind;
  retryable?: boolean;
  device?: RawDevice;
}

export type ActionRequest = { feature: string; action: string; args?: Record<string, unknown> };
export type WriteStateRequest = { feature: string; key: string; value: unknown };
