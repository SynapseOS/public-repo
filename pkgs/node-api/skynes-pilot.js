// Node.js HTTP health-check API for PostgreSQL and Redis
'use strict';
const {createServer} = require('node:http');
const {execFile} = require('node:child_process');
const {promisify} = require('node:util');
const run = promisify(execFile);
const pg = '/usr/lib/postgresql/16/bin/psql';
const redis = '/bin/redis-cli';
const pgHost = process.env.PGHOST || '127.0.0.1';
const pgUser = process.env.PGUSER || 'postgres';
const pgDb = process.env.PGDATABASE || 'postgres';
let active = 0;
createServer(async (req, res) => {
    if (req.url !== '/health' || req.method !== 'GET') {
        res.writeHead(404).end();
        return;
    }
    if (active >= 4) {
        res.writeHead(503).end();
        return;
    }
    active++;
    const options = {timeout: 3000, maxBuffer: 16384};
    try {
        const args = ['-XAtw', '-v', 'ON_ERROR_STOP=1', '-h', pgHost,
            '-U', pgUser, '-d', pgDb, '-c', 'SELECT 1'];
        const [{stdout: redisReply}, {stdout: postgresReply}] = await Promise.all([
            run(redis, ['--raw', 'PING'], options),
            run(pg, args, options),
        ]);
        res.writeHead(200, {'Content-Type': 'application/json'}).end(JSON.stringify({
            redis: redisReply.trim(), postgres: Number(postgresReply.trim()),
        }));
    } catch (error) {
        console.error(error.message);
        res.writeHead(503).end();
    } finally {
        active--;
    }
}).listen(3000, '127.0.0.1');
