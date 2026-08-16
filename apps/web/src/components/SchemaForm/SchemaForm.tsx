import { useEffect, useState } from 'react';
import styles from './SchemaForm.module.css';

// Layer 1 of the config-flow parser: turn a JSON Schema fragment into
// an auto-form. Deliberately small — covers the fields keystone
// plugins actually need (matter's fabricLabel, dirigera's hubUrl /
// token / PEM). More expressive layouts (steps, tabs, conditional
// visibility) belong in Layer 2 in a follow-up.
//
// Supported JSON Schema features:
//  - type: "string" | "integer" | "number" | "boolean" | "object"
//  - enum → <select>
//  - format: "password" | "textarea" → richer string inputs
//  - description → helper text under the label
//  - required → asterisk on the label and a native browser hint
//
// Anything richer (arrays, oneOf, allOf, $ref) falls back to a
// read-only JSON textarea so an operator can still edit unusual
// schemas by hand.

export interface JSONSchema {
  type?: string;
  title?: string;
  description?: string;
  properties?: Record<string, JSONSchema>;
  required?: string[];
  enum?: unknown[];
  format?: string;
  minimum?: number;
  maximum?: number;
  default?: unknown;
}

export interface SchemaFormProps {
  schema: JSONSchema;
  value: Record<string, unknown>;
  onSubmit: (next: Record<string, unknown>) => void;
  submitting?: boolean;
  submitLabel?: string;
}

export function SchemaForm({
  schema,
  value,
  onSubmit,
  submitting,
  submitLabel = 'Сохранить',
}: SchemaFormProps) {
  const [draft, setDraft] = useState(value);
  useEffect(() => setDraft(value), [value]);

  if (!schema || schema.type !== 'object' || !schema.properties) {
    return <FallbackJson value={value} onSubmit={onSubmit} submitting={submitting} submitLabel={submitLabel} />;
  }

  const required = new Set(schema.required ?? []);

  return (
    <form
      className={styles.form}
      onSubmit={(e) => {
        e.preventDefault();
        onSubmit(draft);
      }}
    >
      {Object.entries(schema.properties).map(([key, sub]) => (
        <Field
          key={key}
          name={key}
          schema={sub}
          required={required.has(key)}
          value={draft[key]}
          onChange={(v) => setDraft({ ...draft, [key]: v })}
        />
      ))}
      <button type="submit" className={styles.submit} disabled={submitting}>
        {submitting ? 'Сохраняем…' : submitLabel}
      </button>
    </form>
  );
}

function Field({
  name,
  schema,
  required,
  value,
  onChange,
}: {
  name: string;
  schema: JSONSchema;
  required: boolean;
  value: unknown;
  onChange: (v: unknown) => void;
}) {
  const label = schema.title ?? name;
  const description = schema.description;
  const enumValues = schema.enum;
  const t = schema.type;

  return (
    <label className={styles.field}>
      <span className={styles.label}>
        {label}
        {required && <span aria-hidden> *</span>}
      </span>
      {description && <span className={styles.description}>{description}</span>}
      {renderInput({ name, schema, required, value, onChange, enumValues, t })}
    </label>
  );
}

function renderInput({
  name,
  schema,
  required,
  value,
  onChange,
  enumValues,
  t,
}: {
  name: string;
  schema: JSONSchema;
  required: boolean;
  value: unknown;
  onChange: (v: unknown) => void;
  enumValues?: unknown[];
  t?: string;
}) {
  if (enumValues) {
    return (
      <select
        className={styles.input}
        value={String(value ?? schema.default ?? '')}
        required={required}
        onChange={(e) => onChange(coerce(e.target.value, t))}
      >
        {!required && <option value="">—</option>}
        {enumValues.map((v) => (
          <option key={String(v)} value={String(v)}>
            {String(v)}
          </option>
        ))}
      </select>
    );
  }

  switch (t) {
    case 'boolean':
      return (
        <input
          className={styles.checkbox}
          type="checkbox"
          checked={Boolean(value ?? schema.default ?? false)}
          onChange={(e) => onChange(e.target.checked)}
        />
      );
    case 'integer':
    case 'number':
      return (
        <input
          className={styles.input}
          type="number"
          value={value === undefined || value === null ? '' : String(value)}
          required={required}
          min={schema.minimum}
          max={schema.maximum}
          step={t === 'integer' ? 1 : 'any'}
          onChange={(e) => {
            const raw = e.target.value;
            if (raw === '') { onChange(undefined); return; }
            const n = Number(raw);
            onChange(Number.isNaN(n) ? raw : n);
          }}
        />
      );
    case 'string':
    default:
      if (schema.format === 'textarea') {
        return (
          <textarea
            className={styles.textarea}
            value={value === undefined || value === null ? '' : String(value)}
            required={required}
            rows={4}
            onChange={(e) => onChange(e.target.value)}
          />
        );
      }
      return (
        <input
          className={styles.input}
          type={schema.format === 'password' ? 'password' : 'text'}
          value={value === undefined || value === null ? '' : String(value)}
          required={required}
          autoComplete={schema.format === 'password' ? 'off' : undefined}
          onChange={(e) => onChange(e.target.value)}
          name={name}
        />
      );
  }
}

function coerce(raw: string, t?: string): unknown {
  if (raw === '') return undefined;
  if (t === 'integer') {
    const n = parseInt(raw, 10);
    return Number.isNaN(n) ? raw : n;
  }
  if (t === 'number') {
    const n = Number(raw);
    return Number.isNaN(n) ? raw : n;
  }
  if (t === 'boolean') return raw === 'true';
  return raw;
}

// FallbackJson is the escape hatch for schemas Layer 1 does not handle
// (arrays, oneOf, refs). An operator can still edit their config as
// raw JSON, and the server-side JSON-Schema validator still catches
// invalid payloads on PUT.
function FallbackJson({
  value,
  onSubmit,
  submitting,
  submitLabel,
}: {
  value: unknown;
  onSubmit: (v: Record<string, unknown>) => void;
  submitting?: boolean;
  submitLabel: string;
}) {
  const [raw, setRaw] = useState(JSON.stringify(value ?? {}, null, 2));
  const [err, setErr] = useState<string | null>(null);
  return (
    <form
      className={styles.form}
      onSubmit={(e) => {
        e.preventDefault();
        try {
          const parsed = JSON.parse(raw);
          setErr(null);
          onSubmit(parsed);
        } catch (ex) {
          setErr(String(ex));
        }
      }}
    >
      <p className={styles.description}>
        Схема слишком сложная для автоформы — редактируйте JSON напрямую. Сервер валидирует по JSON Schema
        при сохранении.
      </p>
      <textarea
        className={styles.textarea}
        value={raw}
        rows={12}
        spellCheck={false}
        onChange={(e) => setRaw(e.target.value)}
      />
      {err && <p className={styles.error}>{err}</p>}
      <button type="submit" className={styles.submit} disabled={submitting}>
        {submitting ? 'Сохраняем…' : submitLabel}
      </button>
    </form>
  );
}
