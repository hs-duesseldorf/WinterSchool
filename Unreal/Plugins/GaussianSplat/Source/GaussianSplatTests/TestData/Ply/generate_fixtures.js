// One-off generator used to author the tiny binary .ply fixtures in this folder.
// Not part of the build; kept here purely so the fixtures can be regenerated/audited later.
// Run with: node generate_fixtures.js
const fs = require('fs');
const path = require('path');

const HEADER_PROPERTIES = [
	'x', 'y', 'z',
	'opacity',
	'scale_0', 'scale_1', 'scale_2',
	'rot_0', 'rot_1', 'rot_2', 'rot_3',
];

function buildHeader(numSplats)
{
	const lines = [
		'ply',
		'format binary_little_endian 1.0',
		`element vertex ${numSplats}`,
	];
	for (const Name of HEADER_PROPERTIES)
	{
		lines.push(`property float ${Name}`);
	}
	lines.push('end_header');
	return Buffer.from(lines.join('\n') + '\n', 'ascii');
}

function buildVertex(x, y, z)
{
	// [x, y, z, opacity, scale_0..2, rot_0..3] as 11 little-endian float32s.
	// opacity/scale are left at 0 (post-import: opacity=sigmoid(0)=0.5, scale=exp(0)=1).
	// rot_0 (=> Rotation.W) = 1 with rot_1..3 = 0 gives an identity quaternion.
	const values = [x, y, z, 0, 0, 0, 0, 1, 0, 0, 0];
	const buf = Buffer.alloc(values.length * 4);
	values.forEach((v, i) => buf.writeFloatLE(v, i * 4));
	return buf;
}

const outDir = __dirname;

// Minimal.ply: 4 valid splats forming a unit-ish cross, used as-is for a successful-import
// sanity check and as the basis for Truncated.ply below.
{
	const header = buildHeader(4);
	const body = Buffer.concat([
		buildVertex(0, 0, 0),
		buildVertex(1, 0, 0),
		buildVertex(0, 1, 0),
		buildVertex(0, 0, 1),
	]);
	fs.writeFileSync(path.join(outDir, 'Minimal.ply'), Buffer.concat([header, body]));
}

// Truncated.ply: same header (declares 4 vertices) but the binary payload is chopped short
// (only 1.5 vertices worth of bytes). Exercises UGaussianSplatFactory's "malformed PLY, not
// enough bytes for the declared vertex count" guard.
{
	const header = buildHeader(4);
	const fullBody = Buffer.concat([
		buildVertex(0, 0, 0),
		buildVertex(1, 0, 0),
		buildVertex(0, 1, 0),
		buildVertex(0, 0, 1),
	]);
	const truncatedBody = fullBody.subarray(0, Math.floor(fullBody.length * 1.5 / 4));
	fs.writeFileSync(path.join(outDir, 'Truncated.ply'), Buffer.concat([header, truncatedBody]));
}

// ZeroVertex.ply: well-formed header declaring zero vertices, no body. Exercises the
// "NumSplats == 0" contract of FGaussianSplatBuilder::IsValid() / the factory's empty-asset path.
{
	const header = buildHeader(0);
	fs.writeFileSync(path.join(outDir, 'ZeroVertex.ply'), header);
}

// Corrupt.ply: doesn't even start with the required "ply" magic line, so it fails at the very
// first header check UGaussianSplatFactory performs.
{
	fs.writeFileSync(path.join(outDir, 'Corrupt.ply'), Buffer.from('This is not a PLY file at all.\nJust garbage bytes.\n', 'ascii'));
}

console.log('Fixtures written to', outDir);
