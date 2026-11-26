import { useState } from 'react';
import { StyleSheet, Text, View } from './kit';
import type { Commands } from '../hooks/useCommands';
import {
  PRESENCE_PHONE_NAME,
  presenceReportArgs,
  presenceState,
  presenceText,
  presenceTitle,
} from '../model/presence';
import type { HomeState } from '../protocol/store';
import { Button, Chip, ChipRow, ErrorText } from './controls';
import { Glyph } from './Glyph';
import { useTheme } from './theme';

/**
 * The phone's presence: where its owner is.
 *
 * The card is also the manual override. The geofence (see
 * `docs/automations.md` §5.2) can be wrong — the neighbour's house is 100 m
 * away, the house is L-shaped, the phone is in the car in the driveway — so
 * "I'm home" and "I'm away" are always one tap away.
 *
 * The hub remembers the phone by name, and each phone is its own entity, so two
 * phones in one house are two rules.
 */
export function PresenceCard({ home, commands }: { home: HomeState; commands: Commands }) {
  const t = useTheme();
  const [error, setError] = useState<string | null>(null);
  const state = presenceState(home.entities);
  const title = presenceTitle(home.entities.find((e) => e.device === 'phone'));

  const report = async (homeNow: boolean) => {
    setError(null);
    const r = await commands.request('presence.report', presenceReportArgs(PRESENCE_PHONE_NAME, homeNow));
    if (r === null) setError('The hub did not accept the report.');
  };

  return (
    <View
      accessibilityLabel="Presence"
      style={[styles.card, { backgroundColor: t.card, borderColor: t.border, borderRadius: t.cardRadius }]}
    >
      <View style={styles.head}>
        <Glyph name="phone" size={20} color={t.muted} />
        <Text style={[styles.h2, { color: t.text }]}>Presence</Text>
      </View>
      <Text style={{ color: t.muted }}>
        {title} — {presenceText(state)}
      </Text>
      {state === 'unknown' && (
        <Text style={{ color: t.muted, fontSize: 11.5, marginTop: 2 }}>
          The phone has not reported yet. A rule that watches it stays quiet until it does.
        </Text>
      )}
      <ErrorText text={error} />
      <View style={styles.row}>
        <ChipRow>
          <Chip
            label="I'm home"
            a11y="I am home"
            selected={state === 'home'}
            onPress={() => void report(true)}
          />
          <Chip
            label="I'm away"
            a11y="I am away"
            selected={state === 'away'}
            onPress={() => void report(false)}
          />
        </ChipRow>
      </View>
      {home.entities.some((e) => e.device === 'phone') && (
        <View style={styles.row}>
          <Button
            kind="plain"
            size="compact"
            label="Forget this phone"
            a11y="Forget this phone"
            onPress={() => {
              const phone = home.entities.find((e) => e.device === 'phone');
              if (phone) void commands.request('presence.remove', { entity: phone.id });
            }}
          />
        </View>
      )}
    </View>
  );
}

const styles = StyleSheet.create({
  card: { borderWidth: 1, padding: 14, marginBottom: 12, gap: 4 },
  head: { flexDirection: 'row', alignItems: 'center', gap: 8 },
  h2: { fontSize: 15, fontWeight: '700' },
  row: { flexDirection: 'row', alignItems: 'center', gap: 8, marginTop: 8 },
});
