import { log } from './log.mjs';
log.push('child start');
await undefined;
throw new RangeError('child failed');
