export const add: (a: number, b: number) => number;

/**
 * Runs the library-loading probe. Detailed results are written to hilog
 * under tag "Blender_NAPI"; the returned string is a one-line summary.
 */
export const loadBlenderLibs: () => string;

/**
 * Extract <archive> from HAP rawfile/ into <extractTo>. Returns a one-line
 * summary. A .stamp file inside extractTo makes repeat calls no-ops unless
 * force==true or the archive size changed since the last extraction.
 */
export const extractRawfile: (
    resMgr: object,
    archive: string,
    extractTo: string,
    force: boolean
) => string;

export const setPublicHome: (path: string) => boolean;
export const notifyDatafilesReady: () => void;
export const isBlenderReady: () => boolean;