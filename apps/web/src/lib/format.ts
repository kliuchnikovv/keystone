export function kelvinTone(k: number): 'тёплый' | 'нейтральный' | 'холодный' {
  if (k <= 2700) return 'тёплый';
  if (k <= 3500) return 'нейтральный';
  return 'холодный';
}

export function percent(v: number, digits = 0): string {
  return `${v.toFixed(digits)}%`;
}

export function watts(v: number): string {
  return `${Math.round(v)} W`;
}

export function minutesAgo(isoOrDate: string | Date | undefined): string {
  if (!isoOrDate) return '';
  const t = typeof isoOrDate === 'string' ? new Date(isoOrDate).getTime() : isoOrDate.getTime();
  const min = Math.floor((Date.now() - t) / 60000);
  if (min < 1) return 'сейчас';
  if (min < 60) return `${min} мин`;
  const h = Math.floor(min / 60);
  return `${h} ч`;
}
