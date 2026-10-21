'use strict';
const path=require('node:path');
const port=process.argv[2]||'8769';
process.argv[2]=path.resolve(__dirname,'../../Project/Dist/Web');
process.argv[3]=port;
require('../Build/ServeWeb.cjs');
