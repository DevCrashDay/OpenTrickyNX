import { join } from 'node:path';
import { defaultMountain, migrateMountain } from '../src/core/doc/mountain';
import { AUTHORED_MODEL_LEVEL, commitModelEditDoc, createAuthoredModel, modelEditDocFor, modelNumber } from '../src/core/doc/models';
import { appendPatchFromCorners } from '../src/core/mesh/ops';
import { ensurePlacedPropIds } from '../src/core/effects/authoring';
import { exportLevel } from '../src/server/routes/export';

// TESTMTN plus a box prop placed a few times beside the race line (game cm -> editor m: x=-gx/100, y=gz/100, z=-gy/100)
const doc = migrateMountain(JSON.parse(JSON.stringify(defaultMountain())));
doc.name = 'PROPMTN';
const model = createAuthoredModel(doc, 'Crate');
const s = 4, h = 3;
const faces: [number, number, number][][] = [
  [[0, h, 0], [s, h, 0], [s, h, s], [0, h, s]],
  [[0, 0, 0], [s, 0, 0], [s, h, 0], [0, h, 0]],
  [[s, 0, 0], [s, 0, s], [s, h, s], [s, h, 0]],
  [[s, 0, s], [0, 0, s], [0, h, s], [s, h, s]],
  [[0, 0, s], [0, 0, 0], [0, h, 0], [0, h, s]],
];
for (const f of faces) {
  const grown = appendPatchFromCorners(modelEditDocFor(doc, model), f);
  if (!grown.ok) throw new Error('append failed');
  commitModelEditDoc(model, grown.doc);
}
model.anchor = [s / 2, 0, s / 2];
const spots = JSON.parse(process.argv[3]) as [number, number, number][];
doc.props = spots.map(([gx, gy, gz], i) => ({
  level: AUTHORED_MODEL_LEVEL, model: modelNumber(model.id), name: model.name,
  pos: [-gx / 100, gz / 100 - 0.5, -gy / 100], yaw: i * 30, scale: 1,
}));
ensurePlacedPropIds(doc.props);
const r = await exportLevel(doc, { outDir: join(process.argv[2], doc.name) });
console.log(r.log);
