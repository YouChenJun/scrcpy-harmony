export const start: (args: Array<string>) => boolean;
export const stop: () => void;
export const setLauncher: (callback: (name: string, display: number) => void) => void;
export const setLogFile: (path: string) => void;
export const setEndCallback: (callback: (code: number) => void) => void;
export const setReadyCallback: (callback: () => void) => void;
export const isRunning: () => boolean;
