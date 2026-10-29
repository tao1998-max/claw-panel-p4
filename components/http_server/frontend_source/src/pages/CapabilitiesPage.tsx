import { createMemo, createSignal, For, Show, type Component } from 'solid-js';
import { t, tf } from '../i18n';
import type { AppConfig, CapabilityItem } from '../api/client';
import { appCapabilities } from '../state/config';
import { createConfigTab } from '../state/configTab';
import { TabShell } from '../components/layout/TabShell';
import { PageHeader } from '../components/ui/PageHeader';
import { SavePanel } from '../components/ui/SavePanel';
import { Button } from '../components/ui/Button';
import { Switch } from '../components/ui/Switch';
import { Banner } from '../components/ui/Banner';
import { ConfigTable } from '../components/ui/ConfigTable';
import { RestartConfirmModal } from '../components/system/RestartConfirmModal';

const SENTINEL_NONE = '__none__';

type CapForm = {
  llmVisible: string[];
};

function parseSelection(
  serialized: string,
  items: CapabilityItem[],
  defaultPredicate: (item: CapabilityItem) => boolean,
): string[] {
  const raw = (serialized || '').trim();
  if (!raw) {
    return items.filter(defaultPredicate).map((item) => item.group_id);
  }
  if (raw === SENTINEL_NONE || raw === 'none') return [];
  return raw
    .split(',')
    .map((token) => token.trim())
    .filter((token) => token && token !== SENTINEL_NONE && token !== 'none');
}

function serializeSelection(
  selected: string[],
  items: CapabilityItem[],
  defaultPredicate: (item: CapabilityItem) => boolean,
): string {
  if (items.length === 0) return '';
  const set = new Set(selected);
  const defaults = items.filter(defaultPredicate).map((item) => item.group_id);
  if (defaults.length === set.size && defaults.every((id) => set.has(id))) {
    return '';
  }
  if (set.size === 0) return SENTINEL_NONE;
  return Array.from(set).join(',');
}

type FilterMode = 'all' | 'llm';

export const CapabilitiesPage: Component<{ onRestartRequest: () => void }> = (props) => {
  const tab = createConfigTab<CapForm>({
    tab: 'capabilities',
    groups: ['capabilities'],
    toForm: (config: Partial<AppConfig>) => ({
      llmVisible: parseSelection(
        config.llm_visible_cap_groups ?? '',
        appCapabilities(),
        (item) => item.default_llm_visible,
      ),
    }),
    fromForm: (form) => ({
      llm_visible_cap_groups: serializeSelection(
        form.llmVisible,
        appCapabilities(),
        (item) => item.default_llm_visible,
      ),
    }),
    isDirty: (form, baseline) => {
      const a = new Set(form.llmVisible);
      const b = new Set(baseline.llmVisible);
      return !setEqual(a, b);
    },
  });

  const [filter, setFilter] = createSignal<FilterMode>('all');
  const [search, setSearch] = createSignal('');
  const [confirmOpen, setConfirmOpen] = createSignal(false);

  const llmSet = createMemo(() => new Set(tab.form.llmVisible));

  const filtered = createMemo(() => {
    const keyword = search().trim().toLowerCase();
    return appCapabilities().filter((item) => {
      if (filter() === 'llm' && !llmSet().has(item.group_id)) return false;
      if (!keyword) return true;
      return (
        item.group_id.toLowerCase().includes(keyword) ||
        (item.display_name ?? '').toLowerCase().includes(keyword)
      );
    });
  });

  const toggleLlm = (id: string, checked: boolean) => {
    const visible = new Set(tab.form.llmVisible);
    if (checked) visible.add(id);
    else visible.delete(id);
    tab.setForm('llmVisible', Array.from(visible));
  };

  const restoreLlmDefaults = () => {
    tab.setForm(
      'llmVisible',
      appCapabilities()
        .filter((item) => item.default_llm_visible)
        .map((item) => item.group_id),
    );
  };

  const clearLlm = () => {
    tab.setForm('llmVisible', []);
  };

  const handleSave = async () => {
    await tab.save();
    setConfirmOpen(true);
  };

  const summary = () =>
    tf('capSummary', {
      enabled: appCapabilities().length,
      llm: tab.form.llmVisible.length,
      total: appCapabilities().length,
    });

  return (
    <TabShell>
      <PageHeader
        title={t('sectionCapabilities') as string}
        description={t('capabilitiesDescription') as string}
      />
      <div class="px-5 py-4 flex flex-col gap-3 border-b border-[var(--color-border-subtle)]">
        <div class="flex flex-wrap gap-2 items-center">
          <div class="inline-flex rounded-[var(--radius-sm)] border border-[var(--color-border-subtle)] overflow-hidden">
            <For
              each={[
                { id: 'all' as const, label: t('capFilterAll') as string },
                { id: 'llm' as const, label: t('capFilterLlm') as string },
              ]}
            >
              {(item) => (
                <button
                  type="button"
                  class={[
                    'px-3 py-1.5 text-[0.8rem] transition',
                    filter() === item.id
                      ? 'bg-[var(--color-accent)]/15 text-[var(--color-text-primary)]'
                      : 'text-[var(--color-text-secondary)] hover:text-[var(--color-text-primary)] hover:bg-white/5',
                  ].join(' ')}
                  onClick={() => setFilter(item.id)}
                >
                  {item.label}
                </button>
              )}
            </For>
          </div>
          <input
            type="search"
            placeholder={t('capSearchPlaceholder') as string}
            class="flex-1 min-w-[160px] max-w-xs"
            value={search()}
            onInput={(event) => setSearch(event.currentTarget.value)}
          />
          <div class="flex flex-wrap gap-2">
            <Button size="sm" variant="secondary" onClick={restoreLlmDefaults}>
              {t('capabilityLlmDefaults')}
            </Button>
            <Button size="sm" variant="secondary" onClick={clearLlm}>
              {t('capabilityLlmClear')}
            </Button>
          </div>
        </div>
        <p class="text-[0.78rem] text-[var(--color-text-muted)] m-0">{summary()}</p>
      </div>

      <div class="p-5">
        <Show when={appCapabilities().length === 0}>
          <Banner kind="info" message={t('capabilityLoading') as string} />
        </Show>
        <Show when={appCapabilities().length > 0 && filtered().length === 0}>
          <Banner kind="info" message={t('capabilityNoResult') as string} />
        </Show>
        <ConfigTable
          columns={[
            { label: t('capabilityNameCol') as string },
            { label: t('idCol') as string },
            { label: t('capabilityLlmVisible') as string, class: 'w-28 text-center' },
          ]}
        >
          <For each={filtered()}>
            {(item) => (
              <tr class="border-t border-[var(--color-border-subtle)] hover:bg-white/[0.02]">
                <td class="px-4 py-2.5 text-[0.88rem] text-[var(--color-text-primary)]">
                  <div class="flex flex-col gap-0.5">
                    <span class="font-semibold">{item.display_name || item.group_id}</span>
                  </div>
                </td>
                <td class="px-4 py-2.5 font-mono text-[0.78rem] text-[var(--color-text-muted)] break-all">
                  {item.group_id}
                </td>
                <td class="px-4 py-2.5 text-center">
                  <Switch
                    checked={llmSet().has(item.group_id)}
                    onChange={(checked) => toggleLlm(item.group_id, checked)}
                  />
                </td>
              </tr>
            )}
          </For>
        </ConfigTable>
      </div>

      <SavePanel
        dirty={tab.dirty()}
        saving={tab.saving()}
        onSave={() => handleSave().catch(() => undefined)}
        onDiscard={tab.discard}
        note={t('restartHint') as string}
      />
      <RestartConfirmModal
        open={confirmOpen()}
        onClose={() => setConfirmOpen(false)}
        onConfirm={() => {
          setConfirmOpen(false);
          props.onRestartRequest();
        }}
        subtitle={t('restartHint') as string}
      />
    </TabShell>
  );
};

function setEqual<T>(a: Set<T>, b: Set<T>) {
  if (a.size !== b.size) return false;
  for (const value of a) {
    if (!b.has(value)) return false;
  }
  return true;
}
