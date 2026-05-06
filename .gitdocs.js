const repoName = process.env.GITHUB_REPOSITORY
  ? process.env.GITHUB_REPOSITORY.split('/')[1]
  : ''

module.exports = {
  name: 'QEMU Camp 2026 GPGPU Experiment',
  root: 'docs/gpgpu-experiment',
  output: '.gitdocs_build',
  baseURL: process.env.GITDOCS_BASE_URL || (repoName ? `/${repoName}/` : '/'),
  domain: process.env.GITDOCS_DOMAIN || '',
  crawlable: false,
  breadcrumbs: true,
  prefix_titles: true,
  languages: [
    'asm',
    'bash',
    'c',
    'cpp',
    'ini',
    'json',
    'makefile',
    'text',
    'yaml',
  ],
  header_links: [
    {
      title: 'Repository',
      url: process.env.GITHUB_SERVER_URL && process.env.GITHUB_REPOSITORY
        ? `${process.env.GITHUB_SERVER_URL}/${process.env.GITHUB_REPOSITORY}`
        : 'https://github.com',
    },
  ],
}
