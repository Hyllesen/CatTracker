/**
 * Cat names.
 *
 * A collar is a number - 0, 1, 2 - and a number is not something you call a
 * cat. This keeps the name beside the id in one small JSON file in the app's
 * documents directory, which costs no new dependency: the app already uses
 * react-native-file-access for the walk-test CSV.
 *
 * The file is best-effort. If it cannot be read the app falls back to "Cat 1",
 * "Cat 2", "Cat 3" and carries on - a missing name must never stop the
 * tracker from tracking.
 */

import {Dirs, FileSystem} from 'react-native-file-access';

/** Collar id -> name. */
export type CatNames = Record<string, string>;

const FILE_NAME = 'cats.json';

let cache: CatNames | null = null;

function filePath(): string {
  return `${Dirs.DocumentDir}/${FILE_NAME}`;
}

/** The name shown for a collar that has never been named. */
export function defaultName(id: number): string {
  return `Cat ${id + 1}`;
}

/** The name to show for a collar, falling back to the default. */
export function nameFor(names: CatNames, id: number): string {
  const named = names[String(id)];
  return named && named.trim() ? named.trim() : defaultName(id);
}

export async function loadNames(): Promise<CatNames> {
  if (cache) {
    return cache;
  }
  try {
    const text = await FileSystem.readFile(filePath(), 'utf8');
    const parsed = JSON.parse(text);
    cache =
      parsed && typeof parsed === 'object' && !Array.isArray(parsed)
        ? (parsed as CatNames)
        : {};
  } catch {
    // No file yet, or it is not readable. Both are fine - the defaults work.
    cache = {};
  }
  return cache;
}

/**
 * Save a name for a collar. An empty name removes it, so the collar falls
 * back to "Cat N" again.
 */
export async function saveName(id: number, name: string): Promise<CatNames> {
  const names = {...(await loadNames())};
  const trimmed = name.trim();
  if (trimmed) {
    names[String(id)] = trimmed;
  } else {
    delete names[String(id)];
  }
  cache = names;
  try {
    await FileSystem.writeFile(
      filePath(),
      JSON.stringify(names, null, 2),
      'utf8',
    );
  } catch (e) {
    console.warn('cats: could not write names file', e);
  }
  return names;
}
