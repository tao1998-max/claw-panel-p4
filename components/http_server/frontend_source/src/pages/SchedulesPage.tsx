import { createSignal, For, onCleanup, onMount, Show, type Component } from 'solid-js';
import {
  deleteSchedule,
  fetchSchedules,
  setScheduleEnabled,
  triggerSchedule,
  type ScheduleItem,
} from '../api/client';
import { t } from '../i18n';
import { TabShell } from '../components/layout/TabShell';
import { PageHeader } from '../components/ui/PageHeader';
import { Button } from '../components/ui/Button';
import { Banner } from '../components/ui/Banner';
import { ConfigTable } from '../components/ui/ConfigTable';

function formatRemaining(ms: number): string {
  if (ms <= 0) return '00:00';
  const total = Math.ceil(ms / 1000);
  const days = Math.floor(total / 86400);
  const hours = Math.floor((total % 86400) / 3600);
  const minutes = Math.floor((total % 3600) / 60);
  const seconds = total % 60;
  if (days > 0) return `${days}d ${hours}h`;
  const mm = String(minutes).padStart(2, '0');
  const ss = String(seconds).padStart(2, '0');
  return hours > 0 ? `${hours}:${mm}:${ss}` : `${mm}:${ss}`;
}

export const SchedulesPage: Component = () => {
  const [items, setItems] = createSignal<ScheduleItem[]>([]);
  const [loading, setLoading] = createSignal(true);
  const [error, setError] = createSignal('');
  const [busy, setBusy] = createSignal('');
  const [now, setNow] = createSignal(Date.now());

  let loadingNow = false;
  const load = async () => {
    if (loadingNow) return;
    loadingNow = true;
    try {
      setItems(await fetchSchedules());
      setError('');
    } catch (err) {
      setError((err as Error).message);
    } finally {
      loadingNow = false;
      setLoading(false);
    }
  };

  onMount(() => {
    void load();
    const tick = window.setInterval(() => setNow(Date.now()), 1000);
    const poll = window.setInterval(() => void load(), 5000);
    onCleanup(() => {
      window.clearInterval(tick);
      window.clearInterval(poll);
    });
  });

  const act = async (id: string, fn: () => Promise<unknown>) => {
    if (busy()) return;
    setBusy(id);
    setError('');
    try {
      await fn();
      await load();
    } catch (err) {
      setError((err as Error).message);
    } finally {
      setBusy('');
    }
  };

  const remove = (id: string) => {
    if (!window.confirm(t('scheduleDeleteHint') as string)) return;
    void act(id, () => deleteSchedule(id));
  };

  const nextText = (item: ScheduleItem) => {
    void now();
    if (!item.enabled) return t('scheduleDisabled') as string;
    const next = item.next_fire_ms ?? 0;
    if (!next) return t('scheduleNoNext') as string;
    const remain = next - now();
    if (remain <= 0) return t('scheduleDue') as string;
    return `${t('scheduleNext') as string} ${formatRemaining(remain)}`;
  };

  const kindText = (item: ScheduleItem) => {
    const kind = item.kind ?? '';
    if (kind === 'once') return t('scheduleKindOnce') as string;
    if (kind === 'interval') return t('scheduleKindInterval') as string;
    if (kind === 'cron') return t('scheduleKindCron') as string;
    return kind || '--';
  };

  return (
    <TabShell>
      <PageHeader
        title={t('navSchedules') as string}
        description={t('scheduleDescription') as string}
        actions={
          <Button size="sm" variant="secondary" onClick={() => void load()} disabled={loading()}>
            {t('scheduleRefresh')}
          </Button>
        }
      />
      <Show when={error()}>
        <div class="px-5 pt-4">
          <Banner kind="error" message={error()} />
        </div>
      </Show>
      <Show when={loading() && items().length === 0}>
        <div class="p-5">
          <Banner kind="info" message={t('scheduleLoading') as string} />
        </div>
      </Show>
      <Show when={!loading() && items().length === 0}>
        <div class="p-5">
          <Banner kind="info" message={t('scheduleEmpty') as string} />
        </div>
      </Show>
      <Show when={items().length > 0}>
        <div class="p-5">
          <ConfigTable
            columns={[
              { label: t('scheduleId') as string },
              { label: t('scheduleKind') as string },
              { label: t('scheduleNext') as string },
              { label: t('scheduleRunCount') as string, class: 'w-24 text-center' },
              { label: t('scheduleActions') as string, class: 'w-72 text-right' },
            ]}
          >
            <For each={items()}>
              {(item) => (
                <tr class="border-t border-[var(--color-border-subtle)] hover:bg-white/[0.02]">
                  <td class="px-4 py-2.5">
                    <div class="flex flex-col gap-0.5">
                      <span class="font-mono text-[0.82rem] break-all">{item.id}</span>
                      <Show when={item.event_key}>
                        <span class="text-[0.72rem] text-[var(--color-text-muted)] break-all">
                          {item.event_key}
                        </span>
                      </Show>
                    </div>
                  </td>
                  <td class="px-4 py-2.5 text-[0.82rem] text-[var(--color-text-secondary)]">
                    {kindText(item)}
                  </td>
                  <td class="px-4 py-2.5 text-[0.82rem] text-[var(--color-text-secondary)]">
                    {nextText(item)}
                  </td>
                  <td class="px-4 py-2.5 text-center text-[0.82rem]">{item.run_count ?? 0}</td>
                  <td class="px-4 py-2.5">
                    <div class="flex flex-wrap justify-end gap-1.5">
                      <Button
                        size="xs"
                        variant="secondary"
                        disabled={busy() === item.id}
                        onClick={() => void act(item.id, () => triggerSchedule(item.id))}
                      >
                        {t('scheduleTrigger')}
                      </Button>
                      <Button
                        size="xs"
                        variant="secondary"
                        disabled={busy() === item.id}
                        onClick={() =>
                          void act(item.id, () => setScheduleEnabled(item.id, !item.enabled))
                        }
                      >
                        {item.enabled ? t('scheduleDisable') : t('scheduleEnable')}
                      </Button>
                      <Button
                        size="xs"
                        variant="ghost"
                        disabled={busy() === item.id}
                        onClick={() => remove(item.id)}
                      >
                        {t('scheduleDelete')}
                      </Button>
                    </div>
                  </td>
                </tr>
              )}
            </For>
          </ConfigTable>
        </div>
      </Show>
    </TabShell>
  );
};
