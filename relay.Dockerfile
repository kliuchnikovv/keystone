# The Keystone relay as a container - for Railway or any Docker host.
#
# Build context is the repository root:
#   docker build -f relay.Dockerfile -t keystone-relay .
#   docker run --rm -p 9090:9090 -e RELAY_SECRET=<secret> keystone-relay
#
# The port and the shared secret come from the environment (Railway sets PORT
# itself and passes RELAY_SECRET as a service variable), so the container needs
# no arguments.
FROM node:22-alpine

WORKDIR /srv

# The relay is one file: the ws server plus tsx to run the TypeScript.
COPY app/tools/relay-server.ts ./relay-server.ts
RUN npm init -y >/dev/null \
 && npm install --no-audit --no-fund ws@8.22.0 tsx@4.23.15

ENV NODE_ENV=production
ENV PORT=9090
EXPOSE 9090

USER node
CMD ["node_modules/.bin/tsx", "relay-server.ts"]
