# syntax=docker/dockerfile:1.7

FROM node:20-bookworm-slim AS build
WORKDIR /app
COPY package.json ./
RUN npm install --no-audit --no-fund
COPY tsconfig.json ./
COPY src ./src
RUN npx tsc -p .

FROM node:20-bookworm-slim
WORKDIR /app
ENV NODE_ENV=production
COPY package.json ./
RUN npm install --omit=dev --no-audit --no-fund && npm cache clean --force
COPY --from=build /app/dist ./dist

# Matter needs mDNS/Bonjour on the host network to discover devices and
# talk to the Thread Border Router. Compose is expected to run this with
# `network_mode: host`; the EXPOSE is informational only in that mode.
EXPOSE 5580
VOLUME ["/data"]
ENV KEYSTONE_MATTER_STORAGE=/data

CMD ["node", "dist/index.js"]
