const assert=require('node:assert/strict');
const {extent,series,clockLabel,formalRange}=require('../../web/capture/replay-scope.js');
const data={timeBasis:'common_formal_t0',markers:{formalWindow:[0,2]},radar:[[-1,0,1,2,3],[9,10,11,12,13]],belt:[[-2,0,1.999999999,2,4],[8,20,21,22,23]],comparison:[[-1,0,1,2,3],[1,2,null,4,5],[1,2,3,4,5]]};
assert.deepEqual(extent(data,'all'),[-2,4]);assert.deepEqual(extent(data,'formal'),[0,2]);
assert.deepEqual(series(data,data.radar,'formal'),[[0,1],[10,11]]);
assert.deepEqual(series(data,data.belt,'formal'),[[0,1.999999999],[20,21]]);
assert.deepEqual(series(data,data.comparison,'formal'),[[0,1],[2,null],[2,3]]);
assert.equal(series(data,data.radar,'all'),data.radar); // no mutation, no re-filtering
assert.match(clockLabel(data),/共同正式起点 T0/);
const old={...data,timeBasis:'first_radar_frame',markers:{formalWindow:[]}};
assert.equal(formalRange(old),null);assert.deepEqual(extent(old,'formal'),[-2,4]);
assert.equal(series(old,old.radar,'formal'),old.radar);assert.match(clockLabel(old),/旧会话/);
assert.deepEqual(series(data,[[],[]],'formal'),[[],[]]);
console.log('Replay scope tests passed: boundaries, T1 exclusion, gaps, exports, legacy fallback.');
